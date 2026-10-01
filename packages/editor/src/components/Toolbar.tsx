import { useEffect, useState } from "react";

import { dialogs } from "../lib/dialogs";
import { baseName, recentFiles } from "../lib/files";
import { stepHistory } from "../lib/history";
import { keyHint } from "../lib/keymap";
import { revealError } from "../lib/revealError";
import { useCacheStore } from "../store/cache";
import { summarize, useExecutionStore } from "../store/execution";
import { useGraphStore } from "../store/graph";
import { useManifestStore } from "../store/manifest";
import { useRecipesDirty } from "../store/recipe";
import { useUiStore } from "../store/ui";
import { transport, type LibraryRefresh, type LibrarySettings, type RecentEntry } from "../transport";

import { CommitText } from "./CommitText";
import { RecipeMenu } from "./RecipeMenu";

export interface ToolbarActions {
  onNew: () => void;
  onOpen: () => void;
  onOpenRecent: (path: string) => void;
  onSave: () => void;
  onSaveAs: () => void;
  onRun: () => void;
  onCancel: () => void;
  onLayout: () => void;
}

/** 最近文件下拉（最多 10 条，存在 Tauri 的 app data 里）。 */
function RecentMenu({ onPick }: { onPick: (path: string) => void }) {
  const [open, setOpen] = useState(false);
  const [items, setItems] = useState<RecentEntry[]>([]);

  useEffect(() => {
    if (!open) return;
    void recentFiles().then(setItems);
  }, [open]);

  return (
    <div className="toolbar__recent">
      <button
        type="button"
        data-testid="recent-toggle"
        title="最近打开"
        onClick={() => setOpen((v) => !v)}
      >
        ▾
      </button>
      {open && (
        <div className="toolbar__recentmenu" data-testid="recent-menu">
          {items.length === 0 && <span className="toolbar__recentempty">还没有打开过文件</span>}
          {items.map((r) => (
            <button
              key={r.path}
              type="button"
              data-testid="recent-item"
              title={r.path}
              onClick={() => {
                setOpen(false);
                onPick(r.path);
              }}
            >
              {baseName(r.path)}
            </button>
          ))}
        </div>
      )}
    </div>
  );
}

/** 「将重算 N 个节点」。数字来自 plan_graph，前端不自己推（ADR-0007）。 */
function Recompute() {
  const plan = useCacheStore((s) => s.plan);
  if (plan.size === 0) return null;
  let n = 0;
  for (const node of plan.values()) if (!node.cached) n += 1;
  return (
    <span
      className="toolbar__recompute"
      data-testid="recompute-hint"
      data-count={n}
      title="其余节点会直接复用缓存结果"
    >
      {n === 0 ? "全部命中缓存" : `将重算 ${n} 个节点`}
    </span>
  );
}

/** live preview 的两个旋钮（ADR-0011）。放工具栏是因为它们会改变「运行」的含义。 */
function PreviewControls() {
  const autoRun = useUiStore((s) => s.autoRun);
  const maxPoints = useUiStore((s) => s.previewMaxPoints);
  const previewing = useUiStore((s) => s.previewing);
  return (
    <>
      <label className="toolbar__toggle" title="拖完参数自动补一次正式运行">
        <input
          type="checkbox"
          data-testid="auto-run"
          checked={autoRun}
          onChange={(e) => useUiStore.getState().setAutoRun(e.target.checked)}
        />
        自动运行
      </label>
      <select
        className="toolbar__select"
        data-testid="preview-points"
        value={maxPoints}
        title="预览时源算子抽稀到多少点"
        onChange={(e) => useUiStore.getState().setPreviewMaxPoints(Number(e.target.value))}
      >
        <option value={50_000}>预览 5 万</option>
        <option value={200_000}>预览 20 万</option>
        <option value={1_000_000}>预览 100 万</option>
      </select>
      {previewing && (
        <span className="toolbar__stat toolbar__stat--preview" data-testid="previewing">
          预览中
        </span>
      )}
    </>
  );
}

/** 库算子目录（ADR-0010）：列出、增删、重扫。设置里加的目录 app 与 CLI 读同一份（docs/library-dirs.md），
 *  改了当场保存并重扫。宿主经 HostConfig 整个指定了目录时只读。 */
function LibraryMenu() {
  const [open, setOpen] = useState(false);
  const [busy, setBusy] = useState(false);
  const [settings, setSettings] = useState<LibrarySettings | null>(null);
  const [draft, setDraft] = useState("");
  const [status, setStatus] = useState<{ count: number; problems: string[] } | null>(null);

  const load = async () => {
    try {
      setSettings(await transport.getLibrarySettings());
    } catch (e) {
      useUiStore.getState().showToast(e instanceof Error ? e.message : String(e), "warn");
    }
  };
  useEffect(() => {
    if (open) void load();
  }, [open]);

  const run = async (act: () => Promise<LibraryRefresh>) => {
    setBusy(true);
    try {
      const result = await act();
      useManifestStore.getState().replaceBundle(result.manifest, 0);
      const problems = result.status.problems;
      setStatus({ count: result.status.count, problems });
      useUiStore
        .getState()
        .showToast(
          problems.length === 0
            ? `库里有 ${result.status.count} 个算子`
            : `库算子有 ${problems.length} 个问题：${problems[0]}`,
          problems.length === 0 ? "info" : "warn",
        );
      await load();
    } catch (e) {
      useUiStore.getState().showToast(e instanceof Error ? e.message : String(e), "warn");
    } finally {
      setBusy(false);
    }
  };
  const save = (extra: string[]) => run(() => transport.setLibraryDirs(extra));
  const add = (dir: string) => {
    const d = dir.trim();
    if (!d || !settings) return;
    setDraft("");
    void save([...settings.extraDirs, d]);
  };
  const pickPath = dialogs().pickPath;
  const browse = async () => {
    if (!pickPath) return;
    const d = await pickPath({ mode: "dir" });
    if (d) add(d);
  };

  const editable = settings?.editable === true;
  return (
    <div className="toolbar__recent">
      <button
        type="button"
        data-testid="library-toggle"
        aria-expanded={open}
        title="库算子目录：列出、增删、重扫"
        onClick={() => setOpen((v) => !v)}
      >
        库 ▾
      </button>
      {open && (
        <div className="toolbar__recentmenu library-menu" data-testid="library-menu">
          <div className="library-menu__title">库算子目录</div>
          {settings && (
            <ul className="library-menu__dirs">
              {settings.defaultDir && (
                <li data-testid="library-dir" data-kind="default" title={settings.defaultDir}>
                  <span className="library-menu__path">{settings.defaultDir}</span>
                  <span className="library-menu__tag">默认 ·「保存到库」写这里</span>
                </li>
              )}
              {settings.extraDirs.map((d) => (
                <li key={d} data-testid="library-dir" data-kind={editable ? "extra" : "host"} title={d}>
                  <span className="library-menu__path">{d}</span>
                  {editable && (
                    <button
                      type="button"
                      className="library-menu__remove"
                      data-testid="library-remove"
                      disabled={busy}
                      title="从库目录里去掉（目录本身不删）"
                      onClick={() => void save(settings.extraDirs.filter((x) => x !== d))}
                    >
                      ×
                    </button>
                  )}
                </li>
              ))}
              {settings.envDirs.map((d) => (
                <li key={`env:${d}`} data-testid="library-dir" data-kind="env" title={d}>
                  <span className="library-menu__path">{d}</span>
                  <span className="library-menu__tag">环境变量 LYFLOW_LIBRARY_DIRS</span>
                </li>
              ))}
            </ul>
          )}
          {settings && !editable && (
            <div className="library-menu__note">目录由宿主配置，在这里改不了</div>
          )}
          {editable && (
            <div className="library-menu__add">
              <input
                data-testid="library-dir-input"
                placeholder="粘贴一个目录路径，回车添加"
                value={draft}
                disabled={busy}
                onChange={(e) => setDraft(e.target.value)}
                onKeyDown={(e) => {
                  if (e.key === "Enter") add(draft);
                }}
              />
              <button type="button" data-testid="library-add" disabled={busy || !draft.trim()} onClick={() => add(draft)}>
                添加
              </button>
              {pickPath && (
                <button type="button" data-testid="library-browse" disabled={busy} onClick={() => void browse()}>
                  浏览…
                </button>
              )}
            </div>
          )}
          <div className="library-menu__foot">
            <span data-testid="library-count">
              {status
                ? status.problems.length === 0
                  ? `${status.count} 个库算子`
                  : `${status.problems.length} 个问题`
                : ""}
            </span>
            <button
              type="button"
              data-testid="library-refresh"
              disabled={busy}
              title="重扫库算子目录（库文件是手工放进去的，不重启就生效）"
              onClick={() => void run(() => transport.refreshLibrary())}
            >
              重扫
            </button>
          </div>
          {editable && (
            <div className="library-menu__note">新加的目录下次启动才会自动盯着文件变化；在那之前改了库文件点「重扫」</div>
          )}
        </div>
      )}
    </div>
  );
}

/** 参数面板的开关（param-recipe P2.1）。开着时替代 Inspector。 */
function ParamPanelButton() {
  const open = useUiStore((s) => s.paramPanel.open);
  return (
    <button
      type="button"
      className={open ? "is-on" : undefined}
      data-testid="param-panel-toggle"
      aria-pressed={open}
      onClick={() => useUiStore.getState().toggleParamPanel()}
      title={`参数面板 (${keyHint("paramPanel")})`}
    >
      参数
    </button>
  );
}

function formatDuration(ms: number): string {
  if (ms >= 1000) return `${(ms / 1000).toFixed(2)} s`;
  return `${Math.round(ms)} ms`;
}

/** 运行中的计时器。单独拆出来，免得每秒一次的重渲染波及整个工具栏 ——
 *  尤其是文档名输入框，重渲染会打断输入法的组合状态。 */
function RunClock({ startedAt }: { startedAt: number }) {
  const [now, setNow] = useState(Date.now());
  useEffect(() => {
    const t = setInterval(() => setNow(Date.now()), 100);
    return () => clearInterval(t);
  }, []);
  return <span className="toolbar__elapsed">{formatDuration(now - startedAt)}</span>;
}

function RunControls({ onRun, onCancel }: { onRun: () => void; onCancel: () => void }) {
  const runStatus = useExecutionStore((s) => s.runStatus);
  const startedAt = useExecutionStore((s) => s.startedAt);
  const durationMs = useExecutionStore((s) => s.durationMs);
  const nodes = useExecutionStore((s) => s.nodes);
  const stale = useExecutionStore((s) => s.stale);
  const error = useExecutionStore((s) => s.error);
  const nodeCount = useGraphStore((s) => s.doc.nodes.length);

  const running = runStatus === "running";
  const summary = summarize(nodes);

  return (
    <div className="toolbar__group toolbar__group--run">
      <button
        type="button"
        className="toolbar__run"
        data-testid="run-button"
        onClick={onRun}
        disabled={running || nodeCount === 0}
        title="运行 (F5)"
      >
        ▶ 运行
      </button>
      <button
        type="button"
        className="toolbar__cancel"
        data-testid="cancel-button"
        onClick={onCancel}
        disabled={!running}
        title="取消 (Esc)"
      >
        ■ 取消
      </button>

      {running && startedAt != null && <RunClock startedAt={startedAt} />}
      {!running && durationMs != null && (
        <span className="toolbar__elapsed">{formatDuration(durationMs)}</span>
      )}

      {runStatus !== "idle" && (
        <span
          className={`toolbar__summary toolbar__summary--${runStatus}`}
          data-testid="run-summary"
          data-run-status={runStatus}
        >
          {/* 「done 7 / error 1」—— 一眼看出这次跑成什么样，不用去数节点颜色 */}
          <span className="toolbar__stat toolbar__stat--done">done {summary.done}</span>
          {summary.error > 0 && (
            <button
              type="button"
              className="toolbar__stat toolbar__stat--error toolbar__stat--link"
              data-testid="run-summary-error"
              title={`定位到第一个出错的节点（在子图里也打开进去）。${keyHint("nextError")} / ${keyHint("prevError")} 在出错的节点之间跳`}
              onClick={() => revealError(0)}
            >
              error {summary.error}
            </button>
          )}
          {summary.cancelled > 0 && (
            <span className="toolbar__stat toolbar__stat--cancelled">
              cancelled {summary.cancelled}
            </span>
          )}
          {stale && (
            <span className="toolbar__stat toolbar__stat--stale" title="运行之后图被改过，结果已过时">
              已过时
            </span>
          )}
        </span>
      )}
      <Recompute />
      {error && (
        <span className="toolbar__runerror" title={error}>
          {error}
        </span>
      )}
    </div>
  );
}

export function Toolbar({
  onNew,
  onOpen,
  onOpenRecent,
  onSave,
  onSaveAs,
  onRun,
  onCancel,
  onLayout,
}: ToolbarActions) {
  // 选长度而不是调 canUndo()：函数引用不变，组件不会因为栈变化而重渲染。
  const pastLen = useGraphStore((s) => s.past.length);
  const futureLen = useGraphStore((s) => s.future.length);
  const nextUndo = useGraphStore((s) => s.past[s.past.length - 1]?.label);
  const nextRedo = useGraphStore((s) => s.future[s.future.length - 1]?.label);
  // 脏标记合并显示（K6 ③）：图或任何一个配方有没存的改动都算，Ctrl+S 一次存全
  const graphDirty = useGraphStore((s) => s.dirty);
  const recipesDirty = useRecipesDirty();
  const dirty = graphDirty || recipesDirty;
  const filePath = useGraphStore((s) => s.filePath);
  const name = useGraphStore((s) => s.doc.name);

  return (
    <header className="toolbar">
      <div className="toolbar__group">
        <button type="button" onClick={onNew} title="新建 (Ctrl+N)">新建</button>
        <button type="button" onClick={onOpen} title={`打开 (${keyHint("open")})`}>打开</button>
        <RecentMenu onPick={onOpenRecent} />
        <button type="button" onClick={onSave} title="保存 (Ctrl+S)">保存</button>
        <button type="button" onClick={onSaveAs} title="另存为 (Ctrl+Shift+S)">另存为</button>
      </div>

      <div className="toolbar__group">
        {/* 窄窗口下只留箭头（styles.editor.css 的 1440 断点），字收进 title 与 aria-label */}
        <button
          type="button"
          disabled={pastLen === 0}
          onClick={() => stepHistory("undo")}
          aria-label="撤销"
          title={nextUndo ? `撤销：${nextUndo} (Ctrl+Z)` : "撤销 (Ctrl+Z)"}
        >
          ↶<span className="toolbar__label"> 撤销</span>
        </button>
        <button
          type="button"
          disabled={futureLen === 0}
          onClick={() => stepHistory("redo")}
          aria-label="重做"
          title={nextRedo ? `重做：${nextRedo} (Ctrl+Shift+Z)` : "重做 (Ctrl+Shift+Z)"}
        >
          ↷<span className="toolbar__label"> 重做</span>
        </button>
      </div>

      <div className="toolbar__group">
        <button type="button" onClick={onLayout} title={`整理布局 (${keyHint("layout")})`}>
          整理
        </button>
        <ParamPanelButton />
        <button
          type="button"
          data-testid="drawer-toggle"
          onClick={() => useUiStore.getState().toggleDrawer()}
          title={`日志与诊断 (${keyHint("toggleDrawer")})`}
        >
          抽屉
        </button>
        <LibraryMenu />
        <button
          type="button"
          data-testid="help-toggle"
          onClick={() => useUiStore.getState().setHelpOpen(true)}
          title="快捷键 (?)"
        >
          ?
        </button>
      </div>

      <RunControls onRun={onRun} onCancel={onCancel} />
      <div className="toolbar__group toolbar__group--preview">
        <PreviewControls />
      </div>

      <div className="toolbar__doc">
        {/* 窄窗口下文件名收起，悬停图名看路径 */}
        <CommitText
          className="toolbar__name"
          data-testid="doc-name"
          value={name ?? ""}
          spellCheck={false}
          placeholder="未命名"
          title={filePath ?? undefined}
          onCommit={(text) => useGraphStore.getState().setName(text)}
        />
        {/* 脏标记：没有它用户不知道自己有没有存过（交互清单 P0 #13） */}
        {dirty && (
          <span
            className="toolbar__dirty"
            data-testid="toolbar-dirty"
            title={graphDirty && recipesDirty ? "图与配方都有未保存的改动" : recipesDirty ? "配方有未保存的改动" : "有未保存的改动"}
          >
            ●
          </span>
        )}
        {filePath && (
          <span className="toolbar__path" title={filePath}>
            {baseName(filePath)}
          </span>
        )}
        <RecipeMenu />
      </div>
    </header>
  );
}
