import { ReactFlowProvider, useReactFlow } from "@xyflow/react";
import { MotionConfig } from "motion/react";
import { useCallback, useEffect, useMemo, useRef, useState } from "react";

import { BottomDrawer } from "./components/BottomDrawer";
import { GraphCanvas } from "./components/GraphCanvas";
import { Inspector } from "./components/Inspector";
import { NodePalette } from "./components/NodePalette";
import { NodeSearch } from "./components/NodeSearch";
import { ShortcutPanel } from "./components/ShortcutPanel";
import { Toolbar } from "./components/Toolbar";
import { Viewer3D } from "./components/Viewer3D";
import { useShortcuts } from "./hooks/useShortcuts";
import {
  backupStatus,
  BACKUP_INTERVAL_MS,
  confirmDiscard,
  confirmRestore,
  discardBackup,
  loadDocFrom,
  pickOpenPath,
  pickSavePath,
  readBackup,
  rememberFile,
  saveDocTo,
  suggestFileName,
  writeBackup,
} from "./lib/files";
import { layoutGraph, needsInitialLayout } from "./lib/layout";
import {
  MotionEnabledContext,
  useMotionEnabled,
  usePrefersReducedMotion,
  viewportMs,
  withLayoutTransition,
} from "./lib/motion";
import { fullId, levelOf } from "./lib/subgraph";
import { formatBytes, refreshCacheStats, schedulePlan, useCacheStore } from "./store/cache";
import {
  cancelCurrentRun,
  setRunSceneId,
  startRun,
  subscribeExecutionEvents,
  useExecutionStore,
} from "./store/execution";
import { useGraphStore } from "./store/graph";
import { useManifestStore } from "./store/manifest";
import { useUiStore } from "./store/ui";
import { scheduleValidate } from "./store/validation";
import { setTransport, transport, type Transport } from "./transport";
import { setDialogs, type EditorDialogs } from "./lib/dialogs";
import { hasRelativePathParam } from "./lib/params";
import { isMigration, type MigrationAction } from "./types/execution";
import type { GraphDoc } from "./types/graph";

import "./styles.css";
import "./styles.editor.css";
import "./styles.peek.css";
import "./styles.blocks.css";

const kMinCanvasWidth = 320;

const TRANSPORT_LABEL: Record<string, string> = {
  tauri: "Tauri · 实时",
  http: "HTTP · 实时",
  static: "静态快照",
};

const TRANSPORT_TITLE: Record<string, string> = {
  tauri: "实时读取 C++ 注册表",
  http: "经 HTTP 后端读取 C++ 注册表",
  static: "静态模式：读的是 dump 出来的 manifest 快照，可能过期",
};

function StatusBar() {
  const coreInfo = useManifestStore((s) => s.coreInfo);
  const transportKind = useManifestStore((s) => s.transportKind);
  const path = useUiStore((s) => s.path);
  const doc = useGraphStore((s) => s.doc);
  const level = levelOf(doc, path);
  const nodeCount = level.nodes.length;
  const edgeCount = level.edges.length;
  const selected = useUiStore((s) => s.selectedNodes.size);
  const stats = useCacheStore((s) => s.stats);
  const [libraryCount, setLibraryCount] = useState(0);

  useEffect(() => {
    void transport
      .getLibraryStatus()
      .then((s) => setLibraryCount(s.count))
      .catch(() => setLibraryCount(0));
  }, []);

  return (
    <footer className="statusbar">
      <span className="statusbar__milestone">M4 · 能扩展</span>
      <span>{nodeCount} 节点</span>
      <span>{edgeCount} 连线</span>
      {libraryCount > 0 && (
        <span data-testid="statusbar-library" title="库算子（app data 下的 library/）">
          库 {libraryCount}
        </span>
      )}
      {selected > 0 && <span>已选 {selected}</span>}
      <span className="statusbar__spacer" />
      {stats && (
        <span
          className="statusbar__cache"
          data-testid="statusbar-cache"
          title={`结果缓存 ${stats.entries} 条，预算 ${formatBytes(stats.budgetBytes)}`}
        >
          缓存 {formatBytes(stats.bytes)}
        </span>
      )}
      {coreInfo && (
        <>
          <span>lyflow-core {coreInfo.version}</span>
          <span data-testid="statusbar-operators">{coreInfo.operatorCount} 算子</span>
          {coreInfo.hotReload && (
            <span
              className="statusbar__hot"
              data-testid="statusbar-generation"
              data-generation={coreInfo.generation ?? 0}
              title="开发期热重载已开启：改 C++ 存盘即生效"
            >
              热重载 · 第 {coreInfo.generation ?? 0} 代
            </span>
          )}
        </>
      )}
      <span
        className={`statusbar__transport statusbar__transport--${transportKind}`}
        title={
          TRANSPORT_TITLE[transportKind]
        }
      >
        {TRANSPORT_LABEL[transportKind]}
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
  return (
    <div className={`toast toast--${toast.kind}`} data-testid="toast">
      {toast.text}
    </div>
  );
}

/** 右侧的可拖分栏。一条 4px 把手 + 全局 pointermove，
 *  不引分栏库 —— 一个库的成本是十几 KB 加一套 API，这里只要一个数字。 */
function useDragSplit(
  initial: number,
  min: number,
  max: number,
  container: React.RefObject<HTMLElement | null>,
  reserve: number,
) {
  const [width, setWidth] = useState(initial);
  const dragging = useRef(false);

  useEffect(() => {
    const move = (e: PointerEvent) => {
      if (!dragging.current) return;
      const rect = container.current?.getBoundingClientRect();
      const right = rect ? rect.right : window.innerWidth;
      const limit = rect ? Math.max(min, Math.min(max, rect.width - reserve)) : max;
      const next = right - e.clientX;
      setWidth(Math.max(min, Math.min(limit, next)));
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
  }, [container, min, max, reserve]);

  const onPointerDown = useCallback(() => {
    dragging.current = true;
    document.body.classList.add("is-resizing");
  }, []);

  return { width, onPointerDown };
}

function Workspace({ graphPath, onDocChange, className, theme }: WorkspaceProps) {
  const { screenToFlowPosition, fitView } = useReactFlow();
  const root = useRef<HTMLDivElement>(null);
  const [paletteWidth] = useState(280);
  // 关动效时视口也一步到位（A4）：适配视图、整理之后的 fitView 都不带过渡
  const motionOn = useMotionEnabled();
  const fitMs = viewportMs(motionOn);
  const rightPane = useDragSplit(380, 260, 900, root, paletteWidth + kMinCanvasWidth);

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
    void refreshCacheStats();
  }, []);

  // -- 热重载（ADR-0009）：换 manifest，当前 doc 一个字不动 -------------------
  useEffect(() => {
    let stop: (() => void) | null = null;
    let stopFail: (() => void) | null = null;
    const updated = transport.onManifestUpdated((e) => {
      useManifestStore.getState().replaceBundle(e.manifest, e.generation);
      const graph = useGraphStore.getState();
      useUiStore.getState().showToast(`core 已热重载（${e.operatorCount} 个算子）`);
      // 算子可能被删掉了：重新编译一次，缺算子的节点会在画布上变成「算子缺失」
      void schedulePlan(graph.doc, graph.filePath);
      scheduleValidate(graph.doc, graph.filePath);
    });
    const failed = transport.onCoreReloadFailed((e) => {
      useUiStore.getState().showToast(
        `core 热重载失败，仍在用第 ${e.generation} 代：${e.problems[0] ?? "未知原因"}`,
        "warn",
      );
    });
    void updated.then((fn) => {
      stop = fn;
    });
    void failed.then((fn) => {
      stopFail = fn;
    });
    return () => {
      stop?.();
      stopFail?.();
    };
  }, []);

  // -- 精确 stale（ADR-0007）：doc 每次变就 debounce 重编一次 ------------------
  // 实时校验（m8-plan L16）跟着同一个信号走：错误在拼的时候就标出来，不必等跑完。
  useEffect(() => {
    const graph = useGraphStore.getState();
    void schedulePlan(graph.doc, graph.filePath);
    scheduleValidate(graph.doc, graph.filePath);
    return useGraphStore.subscribe((state, prev) => {
      if (state.doc === prev.doc && state.filePath === prev.filePath) return;
      if (state.doc !== prev.doc) useExecutionStore.getState().markStale();
      schedulePlan(state.doc, state.filePath);
      scheduleValidate(state.doc, state.filePath);
    });
  }, []);

  // -- 每 30 秒写一次 `<file>~` 备份 -----------------------------------------
  useEffect(() => {
    const t = setInterval(() => {
      const graph = useGraphStore.getState();
      // 没存过盘的图没有 `<file>~` 可写；没改过的也不用写
      if (!graph.filePath || !graph.dirty) return;
      void writeBackup(graph.filePath, graph.doc);
    }, BACKUP_INTERVAL_MS);
    return () => clearInterval(t);
  }, []);

  // -- 打开文件的公共尾巴：迁移写回、缺坐标就布局、记最近文件 -----------------
  const afterOpen = useCallback(
    async (doc: GraphDoc, path: string, migrations: MigrationAction[]) => {
      const graph = useGraphStore.getState();
      const ui = useUiStore.getState();
      graph.loadDoc(doc, path);
      ui.clearSelection();

      if (migrations.length > 0) {
        // 一条撤销记录、置 dirty：用户可以撤销掉这次迁移再决定（ADR-0008）
        const n = useGraphStore.getState().applyMigrations(migrations);
        if (n > 0) ui.showToast(`已迁移 ${n} 个节点，保存后生效`);
      }
      // 脚本生成的图必须能打开（graph-doc.md 的承诺）。只在缺坐标时布局，
      // 永远不覆盖用户摆好的位置（E8）。
      if (needsInitialLayout(useGraphStore.getState().doc)) {
        const moves = layoutGraph(useGraphStore.getState().doc);
        useGraphStore.getState().applyLayout(moves);
        setTimeout(() => void fitView({ duration: fitMs }), 50);
      }
      await rememberFile(path);
      ui.showToast(`已打开 ${doc.nodes.length} 个节点`);
    },
    [fitView, fitMs],
  );

  const openPath = useCallback(
    async (path: string) => {
      const ui = useUiStore.getState();
      try {
        // 备份比正文新 = 上次是异常退出的，先问要不要恢复（§2.5）
        const status = await backupStatus(path);
        if (status.newer && (await confirmRestore(path))) {
          const restored = await readBackup(path);
          await afterOpen(
            restored.doc,
            path,
            restored.migrations.filter(isMigration),
          );
          useGraphStore.getState().markSaved(path);
          useUiStore.getState().showToast("已从自动备份恢复，记得保存");
          return;
        }
        if (status.exists) await discardBackup(path);

        const loaded = await loadDocFrom(path);
        await afterOpen(loaded.doc, path, loaded.migrations.filter(isMigration));
      } catch (e) {
        ui.showToast(e instanceof Error ? e.message : String(e), "warn");
      }
    },
    [afterOpen],
  );

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
      await rememberFile(path);
      // 存过盘就没有「未保存的改动」了，备份留着只会在下次开图时误报
      await discardBackup(path);
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
      await openPath(path);
    } catch (e) {
      ui.showToast(e instanceof Error ? e.message : String(e), "warn");
    }
  }, [openPath]);

  const doOpenRecent = useCallback(
    async (path: string) => {
      if (!(await confirmDiscard(useGraphStore.getState().dirty))) return;
      await openPath(path);
    },
    [openPath],
  );

  const doNew = useCallback(async () => {
    const graph = useGraphStore.getState();
    if (!(await confirmDiscard(graph.dirty))) return;
    graph.newDoc();
    useUiStore.getState().clearSelection();
    useCacheStore.getState().reset();
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
      // 目标是**展开后**的路径 id：在子图里点「运行到此节点」也要说得清是哪一个（F2）
      const path = useUiStore.getState().path;
      const full = targets?.map((t) => fullId(path, t));
      await startRun(graph.doc, graph.filePath, { targets: full });
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

  const doLayout = useCallback(() => {
    const graph = useGraphStore.getState();
    const ui = useUiStore.getState();
    const level = levelOf(graph.doc, ui.path);
    const view = { ...graph.doc, nodes: level.nodes, edges: level.edges };
    const moves = layoutGraph(view, ui.selectedNodes.size > 1 ? { only: ui.selectedNodes } : {});
    // 用户触发的整理才过渡（docs/motion-plan.md N4）；打开文件时的初始布局照旧一步到位
    withLayoutTransition(() => graph.applyLayout(moves));
    setTimeout(() => void fitView({ duration: fitMs }), 50);
  }, [fitView, fitMs]);

  const handlers = useMemo(
    () => ({
      onSave: () => void doSave(false),
      onSaveAs: () => void doSave(true),
      onOpen: () => void doOpen(),
      onNew: () => void doNew(),
      onRun: () => void doRun(),
      onCancel: () => void doCancel(),
      onRunToNode: (nodeId: string) => void doRun([nodeId]),
      onRunToSelected: () => {
        const ids = [...useUiStore.getState().selectedNodes];
        if (ids.length === 0) {
          useUiStore.getState().showToast("先选一个节点再按 Shift+F5", "warn");
          return;
        }
        void doRun(ids);
      },
      onLayout: doLayout,
      onFitView: () => void fitView({ duration: fitMs }),
      cursorFlowPosition: () => screenToFlowPosition(cursor.current),
      cursorScreenPosition: () => cursor.current,
    }),
    [doSave, doOpen, doNew, doRun, doCancel, doLayout, fitView, fitMs, screenToFlowPosition],
  );

  useShortcuts(handlers, root);

  // 键盘事件挂在根元素上，所以根元素必须拿得到焦点（A2-3）
  useEffect(() => {
    const el = root.current;
    if (!el) return;
    el.focus({ preventScroll: true });
    const refocus = () => {
      setTimeout(() => {
        const active = el.ownerDocument.activeElement;
        if (!active || active === el.ownerDocument.body) el.focus({ preventScroll: true });
      }, 0);
    };
    // relatedTarget 为空 = 焦点掉回 body，没被别人接走，可以收回来
    const onFocusOut = (e: FocusEvent) => {
      if (e.relatedTarget === null) refocus();
    };
    el.addEventListener("pointerdown", refocus);
    el.addEventListener("focusout", onFocusOut);
    return () => {
      el.removeEventListener("pointerdown", refocus);
      el.removeEventListener("focusout", onFocusOut);
    };
  }, []);

  // 宿主给了初始图就打开它
  const openedRef = useRef<string | null>(null);
  useEffect(() => {
    if (!graphPath || openedRef.current === graphPath) return;
    openedRef.current = graphPath;
    void openPath(graphPath);
  }, [graphPath, openPath]);

  // doc 变化推给宿主
  useEffect(() => {
    if (!onDocChange) return;
    return useGraphStore.subscribe((state, prev) => {
      if (state.doc === prev.doc && state.dirty === prev.dirty) return;
      onDocChange(state.doc, state.dirty);
    });
  }, [onDocChange]);

  // 关动效时 CSS 那一半靠根上的 lyflow-motion-off（系统设置另有 @media 兜底，见 styles.motion.css）
  const rootClass = ["app", motionOn ? "" : "lyflow-motion-off", className ?? ""]
    .filter(Boolean)
    .join(" ");

  return (
    <div
      className={rootClass}
      ref={root}
      tabIndex={-1}
      data-lyflow-editor="1"
      data-motion={motionOn ? "on" : "off"}
      style={theme as React.CSSProperties | undefined}
      onMouseMove={onMouseMove}
    >
      <Toolbar
        onNew={handlers.onNew}
        onOpen={handlers.onOpen}
        onOpenRecent={(p) => void doOpenRecent(p)}
        onSave={handlers.onSave}
        onSaveAs={handlers.onSaveAs}
        onRun={handlers.onRun}
        onCancel={handlers.onCancel}
        onLayout={handlers.onLayout}
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

      <BottomDrawer />
      <StatusBar />
      <NodeSearch />
      <ShortcutPanel />
      <Toast />
    </div>
  );
}

interface WorkspaceProps {
  /** 挂载后自动打开这张图。 */
  graphPath?: string | undefined;
  /** 文档或 dirty 状态变化时回调，给宿主做自己的标题栏/保存提示。 */
  onDocChange?: ((doc: GraphDoc, dirty: boolean) => void) | undefined;
  className?: string | undefined;
  /** `--lyflow-*` 变量的覆盖值，写在编辑器根元素上。 */
  theme?: Record<string, string> | undefined;
}

export interface LyFlowEditorProps extends WorkspaceProps {
  /** 必填。宿主自己 new 一个 TauriTransport / HttpTransport / StaticTransport。 */
  transport: Transport;
  /** 打开/另存/确认对话框。不给就退回 window.confirm，且没有文件选择器。 */
  dialogs?: EditorDialogs | undefined;
  /**
   * 宿主已经加载好的点云会话 id。给了它，「运行」就用那对云跑，而不是让图自己按参数读盘 ——
   * 宿主页面上已经有云的时候（交互测量、数据库页读过点云之后），这是唯一不用改图就能试跑
   * 的办法。不给就是老行为。
   */
  sceneId?: string | null | undefined;
  /** 画布动效（docs/motion-plan.md）。默认开；false 时进出场、闪光、生长、布局过渡全部
   *  跳到终态，CSS 的循环与过渡一并停掉。系统设了「减少动态效果」时等同于 false。
   *  流动的边关了动画仍以静态高亮表示「正在流」。 */
  animations?: boolean | undefined;
}

export function LyFlowEditor({
  transport: t,
  dialogs: d,
  sceneId,
  animations,
  ...rest
}: LyFlowEditorProps) {
  // 装在 render 里而不是 effect 里：子树的 store 一挂载就会去调传输层。
  setTransport(t);
  setDialogs(d);
  setRunSceneId(sceneId ?? null);

  // 动效开关（A4）：宿主关掉，或系统要求减少动效。算好经 context 往下传给节点、连线、画布；
  // motion 自己的组件经 MotionConfig 跳到终态（命令式的 animate 不看它，各处自己查开关）。
  const reducedMotion = usePrefersReducedMotion();
  const motionOn = animations !== false && !reducedMotion;

  // GraphCanvas 和快捷键都要用 useReactFlow（screenToFlowPosition），
  // 所以 Provider 必须包在整个工作区外面，不能只包画布。
  return (
    <MotionEnabledContext.Provider value={motionOn}>
      <MotionConfig reducedMotion={motionOn ? "never" : "always"}>
        <ReactFlowProvider>
          <Workspace {...rest} />
        </ReactFlowProvider>
      </MotionConfig>
    </MotionEnabledContext.Provider>
  );
}
