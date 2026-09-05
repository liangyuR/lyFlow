import { ReactFlowProvider, useReactFlow } from "@xyflow/react";
import { useCallback, useEffect, useMemo, useRef, useState } from "react";

import { GraphCanvas } from "./components/GraphCanvas";
import { Inspector } from "./components/Inspector";
import { NodePalette } from "./components/NodePalette";
import { NodeSearch } from "./components/NodeSearch";
import { Toolbar } from "./components/Toolbar";
import { Viewer3D } from "./components/Viewer3D";
import { useShortcuts } from "./hooks/useShortcuts";
import {
  confirmDiscard,
  loadDocFrom,
  pickOpenPath,
  pickSavePath,
  saveDocTo,
  suggestFileName,
} from "./lib/files";
import {
  cancelCurrentRun,
  startRun,
  subscribeExecutionEvents,
  useExecutionStore,
} from "./store/execution";
import { useGraphStore } from "./store/graph";
import { useManifestStore } from "./store/manifest";
import { useUiStore } from "./store/ui";
import { hasRelativePathParam } from "./lib/params";

function StatusBar() {
  const coreInfo = useManifestStore((s) => s.coreInfo);
  const transportKind = useManifestStore((s) => s.transportKind);
  const nodeCount = useGraphStore((s) => s.doc.nodes.length);
  const edgeCount = useGraphStore((s) => s.doc.edges.length);
  const selected = useUiStore((s) => s.selectedNodes.size);

  return (
    <footer className="statusbar">
      <span className="statusbar__milestone">M2 · 能跑</span>
      <span>{nodeCount} 节点</span>
      <span>{edgeCount} 连线</span>
      {selected > 0 && <span>已选 {selected}</span>}
      <span className="statusbar__spacer" />
      {coreInfo && (
        <>
          <span>lyflow-core {coreInfo.version}</span>
          <span>{coreInfo.operatorCount} 算子</span>
        </>
      )}
      <span
        className={`statusbar__transport statusbar__transport--${transportKind}`}
        title={
          transportKind === "tauri"
            ? "实时读取 C++ 注册表"
            : "浏览器模式：读的是 public/manifest.dev.json 静态快照，可能过期"
        }
      >
        {transportKind === "tauri" ? "Tauri · 实时" : "静态快照"}
      </span>
    </footer>
  );
}

/** 连线被拒绝的原因、保存成功之类的短提示。 */
function Toast() {
  const toast = useUiStore((s) => s.toast);
  const hideToast = useUiStore((s) => s.hideToast);

  useEffect(() => {
    if (!toast) return;
    const t = setTimeout(hideToast, 2600);
    return () => clearTimeout(t);
  }, [toast, hideToast]);

  if (!toast) return null;
  return <div className={`toast toast--${toast.kind}`}>{toast.text}</div>;
}

/**
 * 右侧的可拖分栏。
 *
 * 用一条 4px 的把手 + 全局 pointermove，而不是引一个分栏库：
 * 一个库的成本是十几 KB 加一套自己的 API，这里只要一个数字。
 */
function useDragSplit(initial: number, min: number, max: number) {
  const [width, setWidth] = useState(initial);
  const dragging = useRef(false);

  useEffect(() => {
    const move = (e: PointerEvent) => {
      if (!dragging.current) return;
      const next = window.innerWidth - e.clientX;
      setWidth(Math.max(min, Math.min(max, next)));
    };
    const up = () => {
      dragging.current = false;
      document.body.classList.remove("is-resizing");
    };
    window.addEventListener("pointermove", move);
    window.addEventListener("pointerup", up);
    return () => {
      window.removeEventListener("pointermove", move);
      window.removeEventListener("pointerup", up);
    };
  }, [min, max]);

  const onPointerDown = useCallback(() => {
    dragging.current = true;
    document.body.classList.add("is-resizing");
  }, []);

  return { width, onPointerDown };
}

function Workspace() {
  const { screenToFlowPosition } = useReactFlow();
  const [paletteWidth] = useState(280);
  const rightPane = useDragSplit(380, 260, 900);

  // 粘贴和搜索面板要知道往哪儿放。跟着鼠标走比总是放在画布中心自然得多。
  const cursor = useRef({ x: 0, y: 0 });
  const onMouseMove = useCallback((e: React.MouseEvent) => {
    cursor.current = { x: e.clientX, y: e.clientY };
  }, []);

  const loadManifest = useManifestStore((s) => s.load);
  const manifestStatus = useManifestStore((s) => s.status);
  const manifestError = useManifestStore((s) => s.error);

  useEffect(() => {
    void loadManifest();
  }, [loadManifest]);

  // 连线被 store 拒绝时冒泡成提示
  const rejection = useGraphStore((s) => s.lastRejection);
  useEffect(() => {
    if (!rejection) return;
    useUiStore.getState().showToast(rejection, "warn");
    useGraphStore.getState().clearRejection();
  }, [rejection]);

  // -- 执行事件流 -----------------------------------------------------------
  useEffect(() => {
    void subscribeExecutionEvents();
  }, []);

  // 运行之后图被改过 → 结果标为过时（交互清单 P1 #23）。
  //
  // 订阅的是 doc 的**引用**：graph store 的每个语义化动作都用 immer 产出一份
  // 新 doc，所以引用变了就等于「图被改过」。这比在每个 change 动作里手动打标
  // 可靠得多 —— 后者一定会漏掉将来新加的动作。
  useEffect(() => {
    return useGraphStore.subscribe((state, prev) => {
      if (state.doc !== prev.doc) useExecutionStore.getState().markStale();
    });
  }, []);

  // -- 文件操作 -------------------------------------------------------------
  const doSave = useCallback(async (forcePicker: boolean) => {
    const graph = useGraphStore.getState();
    const ui = useUiStore.getState();
    try {
      let path = graph.filePath;
      if (!path || forcePicker) {
        path = await pickSavePath(path ?? suggestFileName(graph.doc));
        if (!path) return; // 用户取消
      }
      await saveDocTo(path, graph.doc);
      graph.markSaved(path);
      ui.showToast("已保存");
    } catch (e) {
      ui.showToast(e instanceof Error ? e.message : String(e), "warn");
    }
  }, []);

  const doOpen = useCallback(async () => {
    const graph = useGraphStore.getState();
    const ui = useUiStore.getState();
    try {
      if (!(await confirmDiscard(graph.dirty))) return;
      const path = await pickOpenPath();
      if (!path) return;
      const doc = await loadDocFrom(path);
      graph.loadDoc(doc, path);
      ui.clearSelection();
      ui.showToast(`已打开 ${doc.nodes.length} 个节点`);
    } catch (e) {
      ui.showToast(e instanceof Error ? e.message : String(e), "warn");
    }
  }, []);

  const doNew = useCallback(async () => {
    const graph = useGraphStore.getState();
    if (!(await confirmDiscard(graph.dirty))) return;
    graph.newDoc();
    useUiStore.getState().clearSelection();
  }, []);

  // -- 运行 -----------------------------------------------------------------
  const doRun = useCallback(async (targets?: string[]) => {
    const graph = useGraphStore.getState();
    const ui = useUiStore.getState();
    if (graph.doc.nodes.length === 0) {
      ui.showToast("图是空的，先加几个节点", "warn");
      return;
    }
    // 相对路径参数是相对图文件所在目录解析的，没保存过就没有那个目录。
    // 在这里挡下来，比让 core 报「文件不存在: samples/bin.pcd」清楚得多。
    if (
      !graph.filePath &&
      hasRelativePathParam(graph.doc, useManifestStore.getState().operatorsById)
    ) {
      ui.showToast("图里有相对路径参数，请先保存图（相对路径以图文件所在目录为基准）", "warn");
      return;
    }
    try {
      await startRun(graph.doc, graph.filePath, targets);
    } catch (e) {
      ui.showToast(e instanceof Error ? e.message : String(e), "warn");
    }
  }, []);

  const doCancel = useCallback(async () => {
    try {
      await cancelCurrentRun();
    } catch (e) {
      useUiStore.getState().showToast(e instanceof Error ? e.message : String(e), "warn");
    }
  }, []);

  const handlers = useMemo(
    () => ({
      onSave: () => void doSave(false),
      onSaveAs: () => void doSave(true),
      onOpen: () => void doOpen(),
      onNew: () => void doNew(),
      onRun: () => void doRun(),
      onCancel: () => void doCancel(),
      onRunToNode: (nodeId: string) => void doRun([nodeId]),
      cursorFlowPosition: () => screenToFlowPosition(cursor.current),
      cursorScreenPosition: () => cursor.current,
    }),
    [doSave, doOpen, doNew, doRun, doCancel, screenToFlowPosition],
  );

  useShortcuts(handlers);

  return (
    <div className="app" onMouseMove={onMouseMove}>
      <Toolbar
        onNew={handlers.onNew}
        onOpen={handlers.onOpen}
        onSave={handlers.onSave}
        onSaveAs={handlers.onSaveAs}
        onRun={handlers.onRun}
        onCancel={handlers.onCancel}
      />

      <main className="app__body">
        <aside className="app__sidebar" style={{ width: paletteWidth }}>
          {manifestStatus === "loading" && <p className="app__hint">正在读取算子描述…</p>}
          {manifestStatus === "error" && (
            <div className="app__error">
              <strong>读不到算子描述</strong>
              <p>{manifestError}</p>
              <button type="button" onClick={() => void loadManifest()}>
                重试
              </button>
            </div>
          )}
          {manifestStatus === "ready" && <NodePalette />}
        </aside>

        <section className="app__canvas">
          <GraphCanvas onRunToNode={handlers.onRunToNode} />
        </section>

        <div
          className="app__splitter"
          onPointerDown={rightPane.onPointerDown}
          role="separator"
          aria-orientation="vertical"
          data-testid="right-splitter"
        />

        <aside className="app__right" style={{ width: rightPane.width }}>
          {/* 3D 视图在上、参数在下：视觉项目的核心闭环是「改参数 → 看结果」，
              两者离得越近越好（交互清单 P1 #30）。 */}
          <div className="app__viewer">
            <Viewer3D />
          </div>
          <div className="app__inspector">
            <Inspector />
          </div>
        </aside>
      </main>

      <StatusBar />
      <NodeSearch />
      <Toast />
    </div>
  );
}

export default function App() {
  // GraphCanvas 和快捷键都要用 useReactFlow（screenToFlowPosition），
  // 所以 Provider 必须包在整个工作区外面，不能只包画布。
  return (
    <ReactFlowProvider>
      <Workspace />
    </ReactFlowProvider>
  );
}
