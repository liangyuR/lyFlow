// 快捷键的分发。铁律：在输入框里打字时，除了表里标了 inTextField 的什么都不拦。
// 键表本身在 lib/keymap.ts —— 处理器和 `?` 面板都从那一张表生成（E7）。

import { useEffect, type RefObject } from "react";

import { matchShortcut } from "../lib/keymap";
import { subgraphIdOf } from "../types/graph";
import { augmentOperators, fullId, levelOf } from "../lib/subgraph";
import { useManifestStore } from "../store/manifest";
import { copyNodes, deleteHealing, deleteSelection } from "../lib/editActions";
import { stepHistory } from "../lib/history";
import { decodeNodeClipboard } from "../lib/nodeClipboard";
import { stepAlong, type NavDir, type NavHop } from "../lib/nodeRun";
import { revealError } from "../lib/revealError";
import { useCompareStore } from "../store/compare";
import { runControlsOf, useExecutionStore } from "../store/execution";
import { useGraphStore } from "../store/graph";
import { usePeekStore, type PeekWindow } from "../store/peek";
import { useUiStore } from "../store/ui";
import { useModalStore } from "../lib/modal";
import { presetOfShortcut, VIEW_PRESET_EVENT } from "../lib/viewFit";

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

/** 选中一个节点按 Tab 时从它的哪个输出接出：预览里选看的那个（还是它的输出时），否则第一个。没有输出的（写文件这类）null。 */
function branchFrom(nodeId: string): { node: string; port: string } | null {
  const ui = useUiStore.getState();
  const doc = useGraphStore.getState().doc;
  const node = levelOf(doc, ui.path).nodes.find((n) => n.id === nodeId);
  const op = node ? augmentOperators(useManifestStore.getState().operatorsById, doc.subgraphs).get(node.op) : undefined;
  const outputs = op?.outputs ?? [];
  if (outputs.length === 0) return null;
  const picked = ui.viewerPortPick.get(fullId(ui.path, nodeId));
  const port = outputs.find((o) => o.name === picked) ?? outputs[0]!;
  return { node: nodeId, port: port.name };
}

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
  /** 从这个节点接出的新节点放哪（画布坐标）与弹层摆哪（屏幕坐标）；节点不在画布上时 null。 */
  branchSlot: (
    nodeId: string,
    size?: { h: number },
  ) => { flow: { x: number; y: number }; screen: { x: number; y: number } } | null;
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
    // 鼠标此刻停在哪个元素上：1–4 只给鼠标底下那个 3D 预览
    let hovered: Element | null = null;
    const onPointerOver = (e: PointerEvent) => {
      hovered = e.target instanceof Element ? e.target : null;
    };
    const onPointerOut = (e: PointerEvent) => {
      if (e.relatedTarget === null) hovered = null;
    };
    // 键盘沿连线走的上一步（同级之间挪、往回退要看它）；换了层、或者选中换成了别的就不算
    let navHop: (NavHop & { pathKey: string }) | null = null;
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
      if (e.key === "Escape" && ui.viewerMaximized && (e.target as HTMLElement | null)?.tagName === "SELECT") {
        e.preventDefault();
        ui.setViewerMaximized(false);
        return;
      }
      if (e.key === "Escape" && inTextField(e.target)) return;
      if (ui.helpOpen && e.key === "Escape") {
        ui.setHelpOpen(false);
        return;
      }
      // 取消已经发出去了（取消中…）就当没在跑：Esc 回到关查看器窗口、退子图这些平常的用处
      const cancellable = runControlsOf(useExecutionStore.getState()).cancel === "on";
      if (e.key === "Escape" && !cancellable) {
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
      // 预览最大化着：Esc 先还原（浮着的查看器窗口先一个个关，运行要再按一次才取消）。下拉框里的 Esc 也算：
      // 在预览栏的下拉框里选完一项，焦点还在它上面
      if (e.key === "Escape" && ui.viewerMaximized) {
        e.preventDefault();
        // 拦住传播：焦点在画布的节点上时 React Flow 把 Esc 当成「取消选中」，还原回来预览就空了
        e.stopPropagation();
        ui.setViewerMaximized(false);
        return;
      }

      const hit = matchShortcut(e);
      if (!hit) return;
      // 键表说了这个动作在输入框里也响应才响应（F5、F8、存盘这几个打不出字符的）——
      // 用户很可能刚改完参数、焦点还在输入框里就按 F5。
      // 下拉框里没有可撤的文字：Ctrl+Z / Ctrl+Y 归编辑器（以前交给浏览器，选完一项按 Ctrl+Z 什么也不发生）。
      // 字母跳选、方向键、Space 仍归下拉框
      const undoOnSelect = (hit.id === "undo" || hit.id === "redo") && (e.target as HTMLElement | null)?.tagName === "SELECT";
      const viewerOnSelect =
        hit.scope === "viewer" && (e.target as HTMLElement | null)?.tagName === "SELECT" && hovered?.closest("[data-view-presets]") != null;
      if (!hit.inTextField && inTextField(e.target) && !undoOnSelect && !viewerOnSelect) return;
      // 预览最大化着，画布看不见：删除、复制、搜索这些动画布的键不响（不然在看不见的地方删了节点）
      if (ui.viewerMaximized && hit.scope === "canvas") {
        if (hit.id === "search" && onControl(e.target)) return;
        if ((hit.id === "copy" || hit.id === "cut") && hasTextSelection(owner)) return;
        e.preventDefault();
        ui.showToast("预览最大化中：按 Esc 回到画布再编辑");
        return;
      }

      switch (hit.id) {
        case "run":
          e.preventDefault();
          commitFocusedField(e.target);
          handlers.onRun();
          return;
        case "maximizeViewer":
          e.preventDefault();
          // 拦住传播：焦点在画布的节点上时 React Flow 把 Shift+空格当成多选里的取消选中，预览就成了「选中一个节点…」
          e.stopPropagation();
          ui.setViewerMaximized(!ui.viewerMaximized);
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
          if (ui.path.length > 0 && !cancellable) {
            e.preventDefault();
            ui.exitTo(ui.path.length - 1);
            return;
          }
          if (!cancellable) return;
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
        case "duplicate":
        case "duplicateWired": {
          if (ui.selectedNodes.size === 0) return;
          e.preventDefault();
          const result = graph.duplicateNodes([...ui.selectedNodes], { keepInputs: hit.id === "duplicateWired" });
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
          {
            // 只选中了一个节点：新算子接在它的输出后面（预览里选看的那个输出，没选过就是第一个），放在它右边、
            // 并出一条分支；选完画布跟过去、焦点给新节点，接着按 Tab 一路往下接
            // 框选会把相连的线一起选上：选中的线都连着这个节点的，照样算「只选中了一个节点」
            const only = ui.selectedNodes.size === 1 ? [...ui.selectedNodes][0]! : null;
            const edges = only ? levelOf(graph.doc, ui.path).edges : [];
            const ownEdges = [...ui.selectedEdges].every((id) => {
              const ed = edges.find((x) => x.id === id);
              return ed !== undefined && (ed.from.node === only || ed.to.node === only);
            });
            const from = only && ownEdges ? branchFrom(only) : null;
            const slot = from ? handlers.branchSlot(from.node) : null;
            if (from && slot) {
              ui.openSearch({
                ...slot,
                pendingFrom: from,
                pendingSide: "output",
                follow: true,
                place: (size) => handlers.branchSlot(from.node, size)?.flow ?? null,
              });
              return;
            }
          }
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
        case "navUp":
        case "navDown":
        case "navPrev":
        case "navNext": {
          // 拦住传播：React Flow 自己的方向键挪节点不看修饰键。眼下它看到时选中已经换走了（React 先把这次选中刷进去）才没挪，
          // 别指望这个时序 —— 不然 Alt+→ 会把原来那个节点也挪一格、记一条撤销
          e.preventDefault();
          e.stopPropagation();
          if (ui.selectedNodes.size !== 1 || graph.pendingSnapshot) return;
          const cur = [...ui.selectedNodes][0]!;
          const pathKey = ui.path.map((seg) => seg.nodeId).join("/");
          const last = navHop && navHop.pathKey === pathKey && navHop.to === cur ? navHop : null;
          const dir: NavDir = hit.id === "navUp" ? "up" : hit.id === "navDown" ? "down" : hit.id === "navPrev" ? "prev" : "next";
          const step = stepAlong(levelOf(graph.doc, ui.path), cur, dir, last);
          if (!step) return;
          navHop = { ...step.hop, pathKey };
          ui.followNode(step.id);
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
        case "pin": {
          e.preventDefault();
          // 与预览栏的「钉住」按钮同一个开关：钉着就取消；没钉着就钉住选中的那一个
          if (ui.pinnedNode) {
            ui.setPinnedNode(null);
            ui.showToast("已取消钉住：预览重新跟着选中走");
            return;
          }
          const only = ui.selectedNodes.size === 1 ? [...ui.selectedNodes][0]! : null;
          if (!only) {
            ui.showToast("先选中一个节点，再按 P 把预览钉在它上面");
            return;
          }
          ui.setPinnedNode(only);
          ui.showToast("已钉住：选别的节点预览不换，再按 P 取消");
          return;
        }
        case "viewTop":
        case "viewFront":
        case "viewSide":
        case "viewIso": {
          // 鼠标不在 3D 预览上（或是 2D）：不吞这个键
          const host = hovered?.closest("[data-view-presets]");
          const view = presetOfShortcut(hit.id);
          if (!host || !view) return;
          const req = { view, done: false };
          host.dispatchEvent(new CustomEvent(VIEW_PRESET_EVENT, { detail: req }));
          if (req.done) e.preventDefault();
          return;
        }
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
      if (ui.searchPopup || ui.finderOpen || ui.viewerMaximized) return;
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
    owner.addEventListener("pointerover", onPointerOver, true);
    owner.addEventListener("pointerout", onPointerOut, true);
    return () => {
      owner.removeEventListener("pointerover", onPointerOver, true);
      owner.removeEventListener("pointerout", onPointerOut, true);
      el.removeEventListener("keydown", onKeyDown);
      owner.removeEventListener("keydown", onOrphanKeyDown);
      el.removeEventListener("paste", onPaste);
      owner.removeEventListener("paste", onOrphanPaste);
      owner.removeEventListener("pointerdown", onPointerDown, true);
    };
  }, [handlers, root]);
}
