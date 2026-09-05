import { ReactFlowProvider, useReactFlow } from "@xyflow/react";
import { useCallback, useEffect, useMemo, useRef, useState } from "react";

import { GraphCanvas } from "./components/GraphCanvas";
import { Inspector } from "./components/Inspector";
import { NodePalette } from "./components/NodePalette";
import { NodeSearch } from "./components/NodeSearch";
import { Toolbar } from "./components/Toolbar";
import { useShortcuts } from "./hooks/useShortcuts";
import {
  confirmDiscard,
  loadDocFrom,
  pickOpenPath,
  pickSavePath,
  saveDocTo,
  suggestFileName,
} from "./lib/files";
import { useGraphStore } from "./store/graph";
import { useManifestStore } from "./store/manifest";
import { useUiStore } from "./store/ui";

function StatusBar() {
  const coreInfo = useManifestStore((s) => s.coreInfo);
  const transportKind = useManifestStore((s) => s.transportKind);
  const nodeCount = useGraphStore((s) => s.doc.nodes.length);
  const edgeCount = useGraphStore((s) => s.doc.edges.length);
  const selected = useUiStore((s) => s.selectedNodes.size);

  return (
    <footer className="statusbar">
      <span className="statusbar__milestone">M1 · 能编辑</span>
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

function Workspace() {
  const { screenToFlowPosition } = useReactFlow();
  const [paletteWidth] = useState(280);

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

  const handlers = useMemo(
    () => ({
      onSave: () => void doSave(false),
      onSaveAs: () => void doSave(true),
      onOpen: () => void doOpen(),
      onNew: () => void doNew(),
      cursorFlowPosition: () => screenToFlowPosition(cursor.current),
      cursorScreenPosition: () => cursor.current,
    }),
    [doSave, doOpen, doNew, screenToFlowPosition],
  );

  useShortcuts(handlers);

  return (
    <div className="app" onMouseMove={onMouseMove}>
      <Toolbar
        onNew={handlers.onNew}
        onOpen={handlers.onOpen}
        onSave={handlers.onSave}
        onSaveAs={handlers.onSaveAs}
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
          <GraphCanvas />
        </section>

        <aside className="app__inspector">
          <Inspector />
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
