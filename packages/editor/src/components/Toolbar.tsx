import { useCallback, useEffect, useMemo, useRef, useState } from 'react';

import { useDismiss } from '../hooks/useDismiss';
import { usePendingChanges } from '../hooks/usePendingChanges';
import { dialogs } from '../lib/dialogs';
import { baseName, parentName, recentFiles } from '../lib/files';
import { historyRows, jumpHistory, stepHistory } from '../lib/history';
import { keyHint } from '../lib/keymap';
import { revealError } from '../lib/revealError';
import { cleanPathText } from '../lib/params';
import { verdictTally, verdictTone } from '../lib/outputs';
import { diffText, runReadingsOf } from '../lib/runHistory';
import {
  augmentOperators,
  describeEventNode,
  fullId,
  levelOf,
  locateEventNode,
} from '../lib/subgraph';
import { useCacheStore } from '../store/cache';
import { runControlsOf, summarize, useExecutionStore, useJudgedNodes } from '../store/execution';
import { useGraphStore } from '../store/graph';
import { useManifestStore } from '../store/manifest';
import { useRecipeStore, useRecipesDirty } from '../store/recipe';
import { useUiStore } from '../store/ui';
import { usePythonStore } from '../store/python';
import {
  transport,
  type LibraryRefresh,
  type LibrarySettings,
  type RecentEntry,
} from '../transport';

import { CommitText } from './CommitText';
import { RecipeMenu } from './RecipeMenu';
import { PythonLibrary } from './PythonLibrary';

type ToolbarIconName =
  | 'flow'
  | 'plus'
  | 'folder'
  | 'save'
  | 'copy'
  | 'undo'
  | 'redo'
  | 'layout'
  | 'sliders'
  | 'logs'
  | 'library'
  | 'help'
  | 'play'
  | 'stop'
  | 'refresh'
  | 'chevron';

function ToolbarIcon({ name }: { name: ToolbarIconName }) {
  const paths: Record<ToolbarIconName, React.ReactNode> = {
    flow: (
      <>
        <rect x="3" y="3" width="6" height="6" rx="2" />
        <rect x="15" y="15" width="6" height="6" rx="2" />
        <path d="M9 6h6a3 3 0 0 1 3 3v6M6 9v6a3 3 0 0 0 3 3h6" />
      </>
    ),
    plus: <path d="M12 5v14M5 12h14" />,
    folder: (
      <path d="M3 8V6a2 2 0 0 1 2-2h5l2 3h7a2 2 0 0 1 2 2v9a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V8Z" />
    ),
    save: (
      <>
        <path d="M19 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h12l4 4v12a2 2 0 0 1-2 2Z" />
        <path d="M7 3v6h10V3M7 21v-8h10v8" />
      </>
    ),
    copy: (
      <>
        <rect x="8" y="8" width="13" height="13" rx="2" />
        <path d="M16 8V5a2 2 0 0 0-2-2H5a2 2 0 0 0-2 2v9a2 2 0 0 0 2 2h3" />
      </>
    ),
    undo: <path d="m9 5-6 6 6 6M3 11h11a7 7 0 0 1 7 7" />,
    redo: <path d="m15 5 6 6-6 6m6-6H10a7 7 0 0 0-7 7" />,
    layout: (
      <>
        <rect x="3" y="3" width="7" height="7" rx="2" />
        <rect x="14" y="14" width="7" height="7" rx="2" />
        <path d="M14 6h7M17.5 3v7M3 17.5h7M6.5 14v7" />
      </>
    ),
    sliders: (
      <>
        <path d="M4 7h7m4 0h5M4 17h3m4 0h9" />
        <circle cx="13" cy="7" r="2" />
        <circle cx="9" cy="17" r="2" />
      </>
    ),
    logs: (
      <>
        <rect x="3" y="4" width="18" height="16" rx="3" />
        <path d="m7 9 3 3-3 3m6 0h4" />
      </>
    ),
    library: (
      <>
        <path d="M4 4h4v16H4zM11 4h4v16h-4zM18 4l3 15" />
      </>
    ),
    help: (
      <>
        <circle cx="12" cy="12" r="9" />
        <path d="M9.5 9a2.5 2.5 0 0 1 5 0c0 2-2.5 2-2.5 4m0 3h.01" />
      </>
    ),
    play: <path d="m8 4 12 8-12 8V4Z" />,
    stop: <rect x="6" y="6" width="12" height="12" rx="2" />,
    refresh: (
      <>
        <path d="M20 7v5h-5M4 17v-5h5" />
        <path d="M6 7a7 7 0 0 1 12-1l2 6M4 12l2 6a7 7 0 0 0 12-1" />
      </>
    ),
    chevron: <path d="m8 10 4 4 4-4" />,
  };
  return (
    <svg
      className="qs-ic"
      viewBox="0 0 24 24"
      fill="none"
      stroke="currentColor"
      strokeWidth="1.7"
      strokeLinecap="round"
      strokeLinejoin="round"
      aria-hidden="true"
    >
      {paths[name]}
    </svg>
  );
}

export interface ToolbarActions {
  onNew: () => void;
  onOpen: () => void;
  onOpenRecent: (path: string) => void;
  onSave: () => void;
  onSaveAs: () => void;
  onRun: () => void;
  /** 运行中点「↻ 重跑」：同样的范围再来一次。F5 照旧是运行整张图。 */
  onRerun: () => void;
  onCancel: () => void;
  onLayout: () => void;
}

/** 最近文件下拉（最多 10 条，存在 Tauri 的 app data 里）。 */
function RecentMenu({ onPick }: { onPick: (path: string) => void }) {
  const [open, setOpen] = useState(false);
  const [items, setItems] = useState<RecentEntry[]>([]);
  const box = useRef<HTMLDivElement>(null);
  useDismiss(
    open,
    box,
    useCallback(() => setOpen(false), []),
  );

  useEffect(() => {
    if (!open) return;
    void recentFiles().then(setItems);
  }, [open]);

  return (
    <div className="toolbar__recent" ref={box}>
      <button
        type="button"
        className="qs-btn icon sm"
        aria-label="最近打开"
        aria-expanded={open}
        data-testid="recent-toggle"
        title="最近打开"
        onClick={() => setOpen((v) => !v)}
      >
        <ToolbarIcon name="chevron" />
      </button>
      {open && (
        <div className="toolbar__recentmenu qs-popover" data-testid="recent-menu">
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
              <span className="recent__dir">{parentName(r.path)}</span>
            </button>
          ))}
        </div>
      )}
    </div>
  );
}

/** 撤销历史：撤销 / 重做旁边的 ▾。列出做过的每一步（新的在上），存盘的那一步标着，点一行一次走到那里。 */
function HistoryMenu() {
  const [open, setOpen] = useState(false);
  const box = useRef<HTMLDivElement>(null);
  const toggle = useRef<HTMLButtonElement>(null);
  // 收起时焦点若丢了（掉回 body、或还在菜单里）就还给 ▾：键盘接着用，不用再去点
  useDismiss(
    open,
    box,
    useCallback(() => {
      setOpen(false);
      const active = document.activeElement;
      if (!active || active === document.body || box.current?.contains(active))
        toggle.current?.focus();
    }, []),
  );
  const past = useGraphStore((s) => s.past);
  const future = useGraphStore((s) => s.future);
  const doc = useGraphStore((s) => s.doc);
  const savedDoc = useGraphStore((s) => s.savedDoc);
  // 没存过盘的新图：savedDoc 是新建时的那一份，但它从没写进文件，不标「已保存」
  const filePath = useGraphStore((s) => s.filePath);
  const recipes = useRecipeStore((s) => s.set);
  const savedRecipes = useRecipeStore((s) => s.saved);
  const rows = open
    ? historyRows(
        past,
        future,
        { doc, recipes },
        savedDoc && filePath ? { doc: savedDoc, recipes: savedRecipes } : null,
      )
    : [];
  const current = useRef<HTMLButtonElement>(null);
  useEffect(() => {
    if (open) current.current?.scrollIntoView({ block: 'nearest' });
  }, [open]);

  return (
    <div className="toolbar__recent" ref={box}>
      <button
        ref={toggle}
        type="button"
        className="qs-btn icon sm"
        aria-label="撤销历史"
        data-testid="history-toggle"
        aria-expanded={open}
        disabled={past.length === 0 && future.length === 0}
        title="撤销历史：点一行一次走到那一步"
        onClick={() => setOpen((v) => !v)}
      >
        <ToolbarIcon name="chevron" />
      </button>
      {open && (
        <div
          className="toolbar__recentmenu history-menu qs-popover"
          data-testid="history-menu"
          onKeyDown={(e) => {
            if (e.key === 'Escape') {
              e.stopPropagation();
              setOpen(false);
              toggle.current?.focus();
            }
          }}
        >
          {rows.map((r) => (
            <button
              // 按它在整条时间线上的位置给 key（走几步都不变）：点过的那一行不重新挂载，焦点留在它上面
              key={past.length + r.steps}
              ref={r.kind === 'current' ? current : undefined}
              type="button"
              className={`history-menu__row is-${r.kind}`}
              data-testid="history-row"
              data-kind={r.kind}
              data-steps={r.steps}
              data-saved={r.saved ? '1' : undefined}
              aria-current={r.kind === 'current' ? 'step' : undefined}
              title={r.label}
              // 现在这一行点了没用，但不用 disabled：刚点过的那一行变成「现在」时焦点不能丢
              aria-disabled={r.steps === 0 || undefined}
              onClick={() => {
                if (r.steps !== 0) jumpHistory(r.steps, r.label);
              }}
            >
              <span className="history-menu__label">{r.label}</span>
              {r.saved && <span className="history-menu__saved">● 已保存</span>}
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
      {n === 0 ? '全部命中缓存' : `将重算 ${n} 个节点`}
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
      <label
        className="toolbar__toggle"
        title="改完参数就补一次正式运行：拖完松手、敲回车、选下拉框、勾选、↺ 重置、粘贴值、恢复、撤销都算（只补跑过的节点）。关掉时拖动只看抽稀的预览，按 F5 才正式跑"
      >
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
  const [tab, setTab] = useState<'native' | 'python'>('native');
  const pythonRequest = usePythonStore((state) => state.openRequest);
  const pythonBusy = usePythonStore((state) => state.busy);
  useEffect(() => {
    if (pythonRequest > 0) {
      setOpen(true);
      setTab('python');
    }
  }, [pythonRequest]);
  const [busy, setBusy] = useState(false);
  const [settings, setSettings] = useState<LibrarySettings | null>(null);
  const [draft, setDraft] = useState('');
  const [status, setStatus] = useState<{ count: number; problems: string[] } | null>(null);
  const box = useRef<HTMLDivElement>(null);
  useDismiss(
    open,
    box,
    useCallback(() => setOpen(false), []),
  );

  const load = async () => {
    try {
      setSettings(await transport.getLibrarySettings());
    } catch (e) {
      useUiStore.getState().showToast(e instanceof Error ? e.message : String(e), 'warn');
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
          problems.length === 0 ? 'info' : 'warn',
        );
      await load();
    } catch (e) {
      useUiStore.getState().showToast(e instanceof Error ? e.message : String(e), 'warn');
    } finally {
      setBusy(false);
    }
  };
  const save = (extra: string[]) => run(() => transport.setLibraryDirs(extra));
  const add = (dir: string) => {
    // 资源管理器「复制文件地址」给的带引号，与路径参数同一个收拾法
    const d = cleanPathText(dir);
    if (!d || !settings) return;
    setDraft('');
    void save([...settings.extraDirs, d]);
  };
  const pickPath = dialogs().pickPath;
  const browse = async () => {
    if (!pickPath) return;
    try {
      const d = await pickPath({ mode: 'dir' });
      if (d) add(d);
    } catch (e) {
      useUiStore.getState().showToast(e instanceof Error ? e.message : String(e), 'warn');
    }
  };

  const editable = settings?.editable === true;
  return (
    <div className="toolbar__recent" ref={box}>
      <button
        type="button"
        className="qs-btn text sm"
        data-testid="library-toggle"
        aria-expanded={open}
        title="库算子目录：列出、增删、重扫"
        onClick={() => setOpen((v) => !v)}
      >
        <ToolbarIcon name="library" />
        <span>算子库</span>
        <ToolbarIcon name="chevron" />
      </button>
      {open && (
        <div className="toolbar__recentmenu library-menu qs-popover" data-testid="library-menu">
          <div className="library-menu__tabs" role="group" aria-label="库类型">
            <button
              type="button"
              aria-pressed={tab === 'native'}
              disabled={busy || pythonBusy}
              onClick={() => setTab('native')}
              data-testid="library-native-tab"
            >
              库算子
            </button>
            <button
              type="button"
              aria-pressed={tab === 'python'}
              disabled={busy || pythonBusy}
              onClick={() => setTab('python')}
              data-testid="library-python-tab"
            >
              Python 节点
            </button>
          </div>
          {tab === 'python' ? (
            <PythonLibrary />
          ) : (
            <>
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
                    <li
                      key={d}
                      data-testid="library-dir"
                      data-kind={editable ? 'extra' : 'host'}
                      title={d}
                    >
                      <span className="library-menu__path">{d}</span>
                      {editable && (
                        <button
                          type="button"
                          className="library-menu__remove"
                          data-testid="library-remove"
                          disabled={busy || pythonBusy}
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
                    disabled={busy || pythonBusy}
                    onChange={(e) => setDraft(e.target.value)}
                    onKeyDown={(e) => {
                      if (e.key === 'Enter') add(draft);
                    }}
                  />
                  <button
                    type="button"
                    data-testid="library-add"
                    disabled={busy || pythonBusy || !draft.trim()}
                    onClick={() => add(draft)}
                  >
                    添加
                  </button>
                  {pickPath && (
                    <button
                      type="button"
                      data-testid="library-browse"
                      disabled={busy || pythonBusy}
                      onClick={() => void browse()}
                    >
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
                    : ''}
                </span>
                <button
                  type="button"
                  data-testid="library-refresh"
                  disabled={busy || pythonBusy}
                  title="重扫库算子目录（库文件是手工放进去的，不重启就生效）"
                  onClick={() => void run(() => transport.refreshLibrary())}
                >
                  重扫
                </button>
              </div>
              {editable && (
                <div className="library-menu__note">
                  新加的目录下次启动才会自动盯着文件变化；在那之前改了库文件点「重扫」
                </div>
              )}
            </>
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
      className={`qs-btn sm ${open ? 'tonal is-on' : 'text'}`}
      data-testid="param-panel-toggle"
      aria-pressed={open}
      onClick={() => useUiStore.getState().toggleParamPanel()}
      title={`参数面板 (${keyHint('paramPanel')})`}
    >
      <ToolbarIcon name="sliders" />
      <span>参数</span>
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

function RunControls({
  onRun,
  onRerun,
  onCancel,
}: {
  onRun: () => void;
  onRerun: () => void;
  onCancel: () => void;
}) {
  const runStatus = useExecutionStore((s) => s.runStatus);
  // 按字符串订阅：进度事件不让两个按钮重渲
  const [runMode, cancelMode] = useExecutionStore((s) => {
    const c = runControlsOf(s);
    return `${c.run}|${c.cancel}`;
  }).split('|');
  const rerun = runMode === 'rerun';
  const startedAt = useExecutionStore((s) => s.startedAt);
  const durationMs = useExecutionStore((s) => s.durationMs);
  const nodes = useExecutionStore((s) => s.nodes);
  const stale = useExecutionStore((s) => s.stale);
  const error = useExecutionStore((s) => s.error);
  const nodeCount = useGraphStore((s) => s.doc.nodes.length);

  const running = runStatus === 'running';
  const summary = summarize(nodes);

  return (
    <div className="toolbar__group toolbar__group--run">
      <button
        type="button"
        className="toolbar__run qs-btn"
        data-testid="run-button"
        data-mode={runMode}
        onClick={rerun ? onRerun : onRun}
        // 预览与自动补跑的那一次在跑时照旧不能点（拖参数时按钮不闪）
        disabled={(running && !rerun) || nodeCount === 0}
        title={rerun ? '重跑：停掉这一次、按同样的范围重新开始（F5 运行整张图）' : '运行 (F5)'}
      >
        <ToolbarIcon name={rerun ? 'refresh' : 'play'} />
        {rerun ? '重跑' : '运行'}
        <kbd>F5</kbd>
      </button>
      <button
        type="button"
        className="toolbar__cancel qs-btn icon"
        aria-label={cancelMode === 'cancelling' ? '取消中' : '取消'}
        data-testid="cancel-button"
        data-cancelling={cancelMode === 'cancelling' ? '1' : undefined}
        onClick={onCancel}
        disabled={cancelMode !== 'on'}
        title={
          cancelMode === 'cancelling' ? '已经发出取消，等正在算的那个算子停下来' : '取消 (Esc)'
        }
      >
        <ToolbarIcon name="stop" />
      </button>

      {running && startedAt != null && <RunClock startedAt={startedAt} />}
      {!running && durationMs != null && (
        <span className="toolbar__elapsed">{formatDuration(durationMs)}</span>
      )}

      {runStatus !== 'idle' && (
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
              title={`定位到第一个出错的节点（在子图里也打开进去）。${keyHint('nextError')} / ${keyHint('prevError')} 在出错的节点之间跳`}
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
          {stale && <StaleChip />}
          <VerdictTallyChips />
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

/** 这次运行的量测判定一共几个（「NG 2 · 边界 1 · ok 12」）：跑完一眼看出过没过，不用一个个点开节点、也不用进子图找。
 *  点 NG / 边界 打开查找节点（is:ng / is:margin），回车打开到它、Alt+Enter 选上这一层的。拖参数的预览运行不算 ——
 *  那是抽稀的，留着上一次正式运行的计数，不跟着一闪一闪。 */
function VerdictTallyChips() {
  // 拖参数的预览、松手自动补的那一次还在跑：看的是上一次正式运行的节点表（与查找节点的 is:ng 同一份）
  const nodes = useJudgedNodes();
  const doc = useGraphStore((s) => s.doc);
  const baseOps = useManifestStore((s) => s.operatorsById);
  const { tally, ng } = useMemo(() => {
    const readings = runReadingsOf(nodes);
    if (readings.length === 0) return { tally: verdictTally([]), ng: [] as string[] };
    const ops = augmentOperators(baseOps, doc.subgraphs);
    const ng = readings
      .filter((r) => verdictTone(r.verdict) === 'ng')
      .map((r) => `${describeEventNode(doc, ops, r.id).names.join(' › ')}.${r.port}`);
    // 没测出来、但接着的判定节点判了的（量测 → gap.judge）：判定那边已经记成 NG，这一个不再算一次「未测出」
    const judged = new Set(readings.filter((r) => r.verdict).map((r) => r.id));
    const feedsJudge = (r: { id: string; port: string }) => {
      const at = locateEventNode(doc, r.id);
      if (!at || fullId(at.path, at.localId) !== r.id) return false;
      return levelOf(doc, at.path).edges.some(
        (e) =>
          e.from.node === at.localId &&
          e.from.port === r.port &&
          judged.has(fullId(at.path, e.to.node)),
      );
    };
    const counted = readings.filter((r) => r.value !== null || r.verdict || !feedsJudge(r));
    return { tally: verdictTally(counted), ng };
  }, [nodes, doc, baseOps]);
  if (tally.ng + tally.margin + tally.ok + tally.unmeasured === 0) return null;
  const open = (seed: string) => useUiStore.getState().setFinderOpen(true, seed);
  return (
    <>
      {tally.ng > 0 && (
        <button
          type="button"
          className="toolbar__stat toolbar__stat--ng toolbar__stat--link"
          data-testid="run-tally-ng"
          title={`不合格：${ng.join('、')} —— 点一下列出来（回车打开到它）`}
          onClick={() => open('is:ng ')}
        >
          NG {tally.ng}
        </button>
      )}
      {tally.margin > 0 && (
        <button
          type="button"
          className="toolbar__stat toolbar__stat--margin toolbar__stat--link"
          data-testid="run-tally-margin"
          title="接近边界 —— 点一下列出来"
          onClick={() => open('is:margin ')}
        >
          边界 {tally.margin}
        </button>
      )}
      {tally.ok > 0 && (
        <span
          className="toolbar__stat toolbar__stat--ok"
          data-testid="run-tally-ok"
          title="量测判定合格的个数"
        >
          ok {tally.ok}
        </span>
      )}
      {tally.unmeasured > 0 && (
        <span
          className="toolbar__stat toolbar__stat--cancelled"
          data-testid="run-tally-unmeasured"
          title="没测出来的量测输出"
        >
          未测出 {tally.unmeasured}
        </span>
      )}
    </>
  );
}

export function Toolbar({
  onNew,
  onOpen,
  onOpenRecent,
  onSave,
  onSaveAs,
  onRun,
  onRerun,
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
    <header className="toolbar" aria-label="工作流工具栏">
      <div className="toolbar__top qs-top">
        <div className="toolbar__doc">
          <CommitText
            className="toolbar__name"
            data-testid="doc-name"
            value={name ?? ''}
            spellCheck={false}
            placeholder="未命名工作流"
            aria-label="工作流名称"
            title={filePath ?? '工作流名称'}
            onCommit={(text) => useGraphStore.getState().setName(text)}
          />
          {dirty && (
            <span
              className="toolbar__dirty"
              data-testid="toolbar-dirty"
              title={
                graphDirty && recipesDirty
                  ? '图与配方都有未保存的改动'
                  : recipesDirty
                    ? '配方有未保存的改动'
                    : '有未保存的改动'
              }
            >
              ●
            </span>
          )}
          {filePath && (
            <span className="toolbar__path" title={filePath}>
              {baseName(filePath)}
            </span>
          )}
        </div>
        <div className="toolbar__group toolbar__files" role="group" aria-label="文件操作">
          <button className="qs-btn text sm" type="button" onClick={onNew} title="新建 (Ctrl+N)">
            <ToolbarIcon name="plus" />
            <span>新建</span>
          </button>
          <div className="toolbar__group toolbar__open">
            <button
              className="qs-btn text sm"
              type="button"
              onClick={onOpen}
              title={`打开 (${keyHint('open')})`}
            >
              <ToolbarIcon name="folder" />
              <span>打开</span>
            </button>
            <RecentMenu onPick={onOpenRecent} />
          </div>
          <button className="qs-btn tonal sm" type="button" onClick={onSave} title="保存 (Ctrl+S)">
            <ToolbarIcon name="save" />
            <span>保存</span>
          </button>
          <button
            className="qs-btn text sm toolbar__save-as"
            type="button"
            onClick={onSaveAs}
            title="另存为 (Ctrl+Shift+S)"
            aria-label="另存为"
          >
            <ToolbarIcon name="copy" />
            <span>另存为</span>
          </button>
        </div>
        <span className="toolbar__divider" aria-hidden="true" />
        <RecipeMenu />
      </div>
      <div className="toolbar__actions">
        <div className="toolbar__group toolbar__history" role="group" aria-label="编辑历史">
          <button
            className="qs-btn icon sm"
            type="button"
            disabled={pastLen === 0}
            onClick={() => stepHistory('undo')}
            aria-label="撤销"
            title={nextUndo ? `撤销：${nextUndo} (Ctrl+Z)` : '撤销 (Ctrl+Z)'}
          >
            <ToolbarIcon name="undo" />
          </button>
          <button
            className="qs-btn icon sm"
            type="button"
            disabled={futureLen === 0}
            onClick={() => stepHistory('redo')}
            aria-label="重做"
            title={nextRedo ? `重做：${nextRedo} (Ctrl+Shift+Z)` : '重做 (Ctrl+Shift+Z)'}
          >
            <ToolbarIcon name="redo" />
          </button>
          <HistoryMenu />
        </div>
        <span className="toolbar__divider" aria-hidden="true" />
        <div className="toolbar__group toolbar__tools" role="group" aria-label="工作区视图">
          <button
            className="qs-btn text sm"
            type="button"
            onClick={onLayout}
            title={`整理布局 (${keyHint('layout')})`}
          >
            <ToolbarIcon name="layout" />
            <span>整理</span>
          </button>
          <ParamPanelButton />
          <button
            className="qs-btn text sm"
            type="button"
            data-testid="drawer-toggle"
            onClick={() => useUiStore.getState().toggleDrawer()}
            title={`日志与诊断 (${keyHint('toggleDrawer')})`}
          >
            <ToolbarIcon name="logs" />
            <span>日志</span>
          </button>
          <LibraryMenu />
          <button
            className="qs-btn icon sm"
            type="button"
            data-testid="help-toggle"
            aria-label="快捷键帮助"
            onClick={() => useUiStore.getState().setHelpOpen(true)}
            title="快捷键 (?)"
          >
            <ToolbarIcon name="help" />
          </button>
        </div>
        <div className="toolbar__group toolbar__group--preview">
          <PreviewControls />
        </div>
        <RunControls onRun={onRun} onRerun={onRerun} onCancel={onCancel} />
      </div>
    </header>
  );
}

/** 结果过时了：改了参数（静音、增删节点）就写「改 N 处」（与「已过时」差不多宽：1280 宽的窗口里运行区挤满时
 *  长了会被裁掉），悬停写全，点开调参页看是哪几处、能逐条改回；
 *  只动了连线之类比不出来的照旧写「已过时」。 */
function StaleChip() {
  const pending = usePendingChanges();
  if (!pending) {
    return (
      <span className="toolbar__stat toolbar__stat--stale" title="运行之后图被改过，结果已过时">
        已过时
      </span>
    );
  }
  return (
    <button
      type="button"
      className="toolbar__stat toolbar__stat--stale toolbar__stat--link"
      data-testid="run-pending-chip"
      title={`运行之后改了 ${pending.count} 处、还没跑：${diffText(pending.diff, Infinity)}。点开调参页逐条看、改回`}
      onClick={() => {
        const ui = useUiStore.getState();
        if (ui.drawer !== 'runs') ui.toggleDrawer('runs');
      }}
    >
      改 {pending.count} 处
    </button>
  );
}
