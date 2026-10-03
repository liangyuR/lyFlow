import { ReactFlowProvider, useReactFlow } from "@xyflow/react";
import { MotionConfig } from "motion/react";
import { useCallback, useEffect, useMemo, useRef } from "react";

import { BottomDrawer } from "./components/BottomDrawer";
import { GraphCanvas } from "./components/GraphCanvas";
import { Inspector } from "./components/Inspector";
import { NodePalette } from "./components/NodePalette";
import { Modal } from "./components/Modal";
import { NodeFinder } from "./components/NodeFinder";
import { NodeSearch } from "./components/NodeSearch";
import { ParamPanel } from "./components/ParamPanel";
import { ShortcutPanel } from "./components/ShortcutPanel";
import { StatusBar } from "./components/StatusBar";
import { Toast } from "./components/Toast";
import { Toolbar } from "./components/Toolbar";
import { Viewer3D } from "./components/Viewer3D";
import { usePaneLayout } from "./hooks/usePaneLayout";
import { useShortcuts } from "./hooks/useShortcuts";
import {
  autosaveTick,
  discardUntitledBackup,
  findUntitledBackup,
  restoreUntitled,
  untitledRestoreMessage,
} from "./lib/autosave";
import { dialogs } from "./lib/dialogs";
import { saveCurrent } from "./lib/saveFlow";
import { resolveUnsaved } from "./lib/unsaved";
import {
  backupStatus,
  BACKUP_INTERVAL_MS,
  confirmRestore,
  discardBackup,
  loadDocFrom,
  pickOpenPath,
  readBackup,
  rememberFile,
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
import {
  refreshCacheStats,
  requestPlan,
  schedulePlan,
  useCacheStore,
} from "./store/cache";
import {
  cancelCurrentRun,
  setRunSceneId,
  startRun,
  subscribeExecutionEvents,
  useExecutionStore,
} from "./store/execution";
import { useGraphStore } from "./store/graph";
import { useManifestStore } from "./store/manifest";
import { recipesDirty, useRecipeStore } from "./store/recipe";
import {
  discardRecipeAutosave,
  followGraphPath,
  loadRecipesFor,
  restoreRecipeAutosave,
} from "./store/recipeFiles";
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

function Workspace({ graphPath, onDocChange, className, theme }: WorkspaceProps) {
  const { screenToFlowPosition, fitView } = useReactFlow();
  const root = useRef<HTMLDivElement>(null);
  // 关动效时视口也一步到位（A4）：适配视图、整理之后的 fitView 都不带过渡
  const motionOn = useMotionEnabled();
  const fitMs = viewportMs(motionOn);
  const panel = useUiStore((s) => s.paramPanel);
  const layout = usePaneLayout(root, panel);

  // 粘贴和搜索面板要知道往哪儿放。跟着鼠标走比总是放在画布中心自然得多。
  const cursor = useRef({ x: 0, y: 0 });
  const onMouseMove = useCallback((e: React.MouseEvent) => {
    cursor.current = { x: e.clientX, y: e.clientY };
  }, []);
  /** 粘贴、Tab 搜索落在哪：鼠标在画布上就跟着鼠标；不在（停在检查器、工具栏上，或者还没动过）就落在画布中间。
   *  以前一律照鼠标的位置算 —— 在检查器上按 Ctrl+V，节点粘到了检查器底下，画布上看不见。 */
  const canvasPoint = useCallback(() => {
    const c = cursor.current;
    const pane = root.current?.querySelector(".react-flow")?.getBoundingClientRect();
    if (!pane || pane.width === 0) return c;
    const inside = c.x >= pane.left && c.x <= pane.right && c.y >= pane.top && c.y <= pane.bottom;
    return inside ? c : { x: Math.round(pane.left + pane.width / 2), y: Math.round(pane.top + pane.height / 2) };
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
    const stopGraph = useGraphStore.subscribe((state, prev) => {
      if (state.doc === prev.doc && state.filePath === prev.filePath) return;
      if (state.doc !== prev.doc) useExecutionStore.getState().markStale();
      schedulePlan(state.doc, state.filePath);
      scheduleValidate(state.doc, state.filePath);
    });
    // 当前配方的覆盖变了（切配方，或改了当前配方里的值）：doc 没动，但交给 core 的取值变了 ——
    // 计划（stale 与「将重算 N 个」）与诊断跟着重算（K5、P3.8）。切换配方时开着「自动运行」就照常补一次运行
    const stopRecipe = useRecipeStore.subscribe((state, prev) => {
      if (state.overrides === prev.overrides) return;
      const g = useGraphStore.getState();
      useExecutionStore.getState().markStale();
      schedulePlan(g.doc, g.filePath);
      scheduleValidate(g.doc, g.filePath);
      // 只跟着人切的那一下跑：打开图时选中默认配方、撤销把当前配方撤没了，都不该自己跑起来
      if (state.current === prev.current || state.switchedBy !== "user") return;
      const ui = useUiStore.getState();
      const exec = useExecutionStore.getState();
      if (!ui.autoRun || exec.runStatus === "idle" || exec.preview || g.doc.nodes.length === 0) return;
      void startRun(g.doc, g.filePath, {}).catch((e: unknown) => {
        ui.showToast(e instanceof Error ? e.message : String(e), "warn");
      });
    });
    // 配方跟着图走（P3.1）：换了一张图（打开、新建）就读它旁边的 <图名>.recipes/；同一张图只是换了路径
    // （第一次存盘）就只换目录。放在订阅里而不是 openPath 里：宿主与脚本直接 loadDoc 也一样生效
    void loadRecipesFor(graph.filePath);
    const stopFiles = useGraphStore.subscribe((state, prev) => {
      if (state.epoch !== prev.epoch) void loadRecipesFor(state.filePath);
      else if (state.filePath !== prev.filePath) followGraphPath(state.filePath);
    });
    return () => {
      stopGraph();
      stopRecipe();
      stopFiles();
    };
  }, []);

  // 一次正式运行收场就立刻重编一次（修订一 V5）：plan 的 cached 是「现在点下去会不会真算」的
  // 依据，只随 doc 变化重编的话，刚跑完的节点仍被当成「没缓存」，按钮上的预告就是错的
  useEffect(
    () =>
      useExecutionStore.subscribe((state, prev) => {
        if (prev.runStatus !== "running" || state.runStatus === "running" || state.preview) return;
        const graph = useGraphStore.getState();
        void requestPlan(graph.doc, graph.filePath);
      }),
    [],
  );

  // -- 每 30 秒一次自动备份：存过盘的写 `<file>~`，没存过盘的写到 app data 里（lib/autosave.ts）-------
  useEffect(() => {
    const t = setInterval(() => void autosaveTick(), BACKUP_INTERVAL_MS);
    return () => clearInterval(t);
  }, []);

  // -- 开 app 时：上次有一张没存过盘的图没保存就退出了（崩溃、断电、被强杀），问一句要不要恢复 -----------
  const offeredUntitled = useRef(false);
  useEffect(() => {
    if (manifestStatus !== "ready" || offeredUntitled.current) return;
    offeredUntitled.current = true;
    void (async () => {
      const backup = await findUntitledBackup();
      const blank = () => {
        const g = useGraphStore.getState();
        return !g.filePath && !g.dirty && g.doc.nodes.length === 0;
      };
      // 只在还是那张空白的新图时问：宿主可能一开就载入了别的图，那就不打扰（备份留到下次）
      if (!backup || !blank()) return;
      if (!(await dialogs().confirmRestore(backup.path, untitledRestoreMessage(backup)))) {
        await discardUntitledBackup();
        return;
      }
      if (!blank()) return;
      restoreUntitled(backup);
      setTimeout(() => void fitView({ duration: fitMs }), 50);
      useUiStore.getState().showToast(`已恢复上次没存的图（${backup.loaded.doc.nodes.length} 个节点），记得保存`);
    })();
  }, [manifestStatus, fitView, fitMs]);

  // -- 打开文件的公共尾巴：迁移写回、缺坐标就布局、记最近文件 -----------------
  const afterOpen = useCallback(
    async (doc: GraphDoc, path: string, migrations: MigrationAction[]) => {
      const graph = useGraphStore.getState();
      const ui = useUiStore.getState();
      graph.loadDoc(doc, path);
      ui.clearSelection();

      // 一条撤销记录、置 dirty：用户可以撤销掉这次迁移再决定（ADR-0008）
      const migrated = migrations.length > 0 ? useGraphStore.getState().applyMigrations(migrations) : 0;
      // 脚本生成的图必须能打开（graph-doc.md 的承诺）。只在缺坐标时布局，
      // 永远不覆盖用户摆好的位置（E8）。
      if (needsInitialLayout(useGraphStore.getState().doc)) {
        const moves = layoutGraph(useGraphStore.getState().doc);
        useGraphStore.getState().applyLayout(moves);
        setTimeout(() => void fitView({ duration: fitMs }), 50);
      }
      await rememberFile(path);
      // 迁移的那句并进来：以前两条分开弹，「已迁移」紧接着就被「已打开」顶掉
      ui.showToast(`已打开 ${doc.nodes.length} 个节点${migrated > 0 ? `，迁移了 ${migrated} 个（保存后生效）` : ""}`);
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
          // 内容不在盘上：算没保存（以前 markSaved 成「已保存」—— 标题没有 *、关窗口也不问，恢复出来的又丢了）
          useGraphStore.getState().markUnsaved();
          const recipes = await restoreRecipeAutosave();
          useUiStore.getState().showToast(recipes ? "已从自动备份恢复图与配方，记得保存" : "已从自动备份恢复，记得保存");
          return;
        }
        if (status.exists) {
          await discardBackup(path);
          await discardRecipeAutosave(path);
        }

        const loaded = await loadDocFrom(path);
        await afterOpen(loaded.doc, path, loaded.migrations.filter(isMigration));
      } catch (e) {
        ui.showToast(e instanceof Error ? e.message : String(e), "warn");
      }
    },
    [afterOpen],
  );

  // -- 文件操作 -------------------------------------------------------------
  // 保存在 lib/saveFlow：「保存 / 不保存 / 取消」那一问选了保存，也走它
  const doSave = useCallback(async (forcePicker: boolean) => {
    await saveCurrent(forcePicker);
  }, []);

  /** 打开一张图；被换掉的是没存过盘的那张时，它的备份也删掉（用户已经确认过放弃它）。 */
  const openUnlessCancelled = useCallback(
    async (path: string) => {
      const wasUntitled = !useGraphStore.getState().filePath;
      await openPath(path);
      if (wasUntitled && useGraphStore.getState().filePath === path) await discardUntitledBackup();
    },
    [openPath],
  );

  const doOpen = useCallback(async () => {
    const ui = useUiStore.getState();
    try {
      if (!(await resolveUnsaved("打开别的图"))) return;
      const path = await pickOpenPath();
      if (!path) return;
      await openUnlessCancelled(path);
    } catch (e) {
      ui.showToast(e instanceof Error ? e.message : String(e), "warn");
    }
  }, [openUnlessCancelled]);

  const doOpenRecent = useCallback(
    async (path: string) => {
      if (!(await resolveUnsaved("打开别的图"))) return;
      await openUnlessCancelled(path);
    },
    [openUnlessCancelled],
  );

  const doNew = useCallback(async () => {
    if (!(await resolveUnsaved("新建"))) return;
    // 问过之后再取：选了保存，没存过盘的图这时已经有了路径
    const graph = useGraphStore.getState();
    if (!graph.filePath) void discardUntitledBackup();
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
      onFitSelection: () => {
        const ids = [...useUiStore.getState().selectedNodes];
        if (ids.length === 0) {
          useUiStore.getState().showToast("先选中节点再按 F", "warn");
          return;
        }
        // 只选了一个时别放大到糊满屏幕：maxZoom 与「打开到出错的节点」同一档
        void fitView({ nodes: ids.map((id) => ({ id })), duration: fitMs, maxZoom: 1, padding: 0.4 });
      },
      cursorFlowPosition: () => screenToFlowPosition(canvasPoint()),
      cursorScreenPosition: canvasPoint,
    }),
    [doSave, doOpen, doNew, doRun, doCancel, doLayout, fitView, fitMs, screenToFlowPosition, canvasPoint],
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

  // doc 变化推给宿主。dirty 是图与配方合并的（K6 ③）：只改了配方也算有没存的改动
  useEffect(() => {
    if (!onDocChange) return;
    const stopGraph = useGraphStore.subscribe((state, prev) => {
      if (state.doc === prev.doc && state.dirty === prev.dirty) return;
      onDocChange(state.doc, state.dirty || recipesDirty());
    });
    const stopRecipes = useRecipeStore.subscribe((state, prev) => {
      if ((state.set !== state.saved) === (prev.set !== prev.saved)) return;
      const g = useGraphStore.getState();
      onDocChange(g.doc, g.dirty || state.set !== state.saved);
    });
    return () => {
      stopGraph();
      stopRecipes();
    };
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

      <main className={`app__body${panel.open && panel.maximized ? " is-panel-max" : ""}`}>
        <aside className="app__sidebar" style={{ width: layout.paletteWidth }}>
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

        <div
          className="app__splitter app__splitter--left"
          onPointerDown={layout.onPaletteSplitterDown}
          role="separator"
          aria-orientation="vertical"
          title="拖动调整算子面板宽度"
          data-testid="left-splitter"
        />

        <section className="app__canvas">
          <GraphCanvas onRunToNode={handlers.onRunToNode} onOpenRecent={(p) => void doOpenRecent(p)} onLayout={handlers.onLayout} />
        </section>

        <div
          className="app__splitter"
          onPointerDown={layout.onRightSplitterDown}
          role="separator"
          aria-orientation="vertical"
          data-testid="right-splitter"
        />

        <aside
          className={`app__right${panel.open ? " app__right--panel" : ""}`}
          style={{ width: layout.rightWidth }}
          data-testid="right-pane"
          ref={layout.rightCol}
        >
          {/* 3D 视图在上、参数在下：视觉项目的核心闭环是「改参数 → 看结果」，
              两者离得越近越好（交互清单 P1 #30）。参数面板开着时视图收成一条标题栏
              （面板要竖向的地方），ROI 行「拖框」会把它展开。Viewer3D 始终是同一个实例，
              切换时不重建 WebGL 场景。 */}
          {panel.open && (
            <button
              type="button"
              className="app__viewer-toggle"
              data-testid="pp-viewer-toggle"
              aria-expanded={panel.viewerOpen}
              onClick={() => useUiStore.getState().setPanelViewerOpen(!panel.viewerOpen)}
            >
              {panel.viewerOpen ? "▾" : "▸"} 预览
            </button>
          )}
          <div
            className={`app__viewer${layout.viewer.shown ? "" : " is-collapsed"}`}
            ref={layout.viewerBox}
            style={layout.viewer.fraction !== null ? { flexBasis: `${layout.viewer.fraction * 100}%` } : undefined}
          >
            <Viewer3D onRunToNode={handlers.onRunToNode} />
          </div>
          {layout.viewer.shown && (
            <div
              className="app__hsplit"
              onPointerDown={layout.viewer.onPointerDown}
              onDoubleClick={layout.viewer.reset}
              role="separator"
              aria-orientation="horizontal"
              title="拖动调整预览高度，双击恢复默认"
              data-testid="viewer-splitter"
            />
          )}
          {panel.open ? (
            <ParamPanel />
          ) : (
            <div className="app__inspector">
              <Inspector />
            </div>
          )}
        </aside>
      </main>

      <BottomDrawer />
      <StatusBar />
      <NodeSearch />
      <NodeFinder />
      <ShortcutPanel />
      <Modal />
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

/** 当前挂着几个 LyFlowEditor。store 与 transport 都是模块级单例（ADR-0018），第二个会和第一个共用
 *  一份文档、后挂的 transport 顶掉先挂的 —— 静默串台比报错难查得多，所以直接报（docs/multi-instance-research.md）。 */
let mountedEditors = 0;

export function LyFlowEditor({
  transport: t,
  dialogs: d,
  sceneId,
  animations,
  ...rest
}: LyFlowEditorProps) {
  useEffect(() => {
    mountedEditors += 1;
    if (mountedEditors > 1) {
      console.error(
        `[lyflow] 同一页面挂了 ${mountedEditors} 个 <LyFlowEditor>：editor 包目前只支持一页一个（ADR-0018），` +
          "它们会共用同一份文档与 store，后挂载的 transport 会顶掉先挂的。见 docs/embedding.md「一页一个编辑器」",
      );
    }
    return () => {
      mountedEditors -= 1;
    };
  }, []);

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
