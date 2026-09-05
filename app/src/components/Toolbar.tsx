import { useEffect, useState } from "react";

import { baseName, recentFiles } from "../lib/files";
import { keyHint } from "../lib/keymap";
import { useCacheStore } from "../store/cache";
import { summarize, useExecutionStore } from "../store/execution";
import { useGraphStore } from "../store/graph";
import { useUiStore } from "../store/ui";
import type { RecentEntry } from "../transport";

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
            <span className="toolbar__stat toolbar__stat--error">error {summary.error}</span>
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
  const undo = useGraphStore((s) => s.undo);
  const redo = useGraphStore((s) => s.redo);
  // 选长度而不是调 canUndo()：函数引用不变，组件不会因为栈变化而重渲染。
  const pastLen = useGraphStore((s) => s.past.length);
  const futureLen = useGraphStore((s) => s.future.length);
  const nextUndo = useGraphStore((s) => s.past[s.past.length - 1]?.label);
  const nextRedo = useGraphStore((s) => s.future[s.future.length - 1]?.label);
  const dirty = useGraphStore((s) => s.dirty);
  const filePath = useGraphStore((s) => s.filePath);
  const name = useGraphStore((s) => s.doc.name);
  const setName = useGraphStore((s) => s.setName);

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
        <button
          type="button"
          disabled={pastLen === 0}
          onClick={undo}
          title={nextUndo ? `撤销：${nextUndo} (Ctrl+Z)` : "撤销 (Ctrl+Z)"}
        >
          ↶ 撤销
        </button>
        <button
          type="button"
          disabled={futureLen === 0}
          onClick={redo}
          title={nextRedo ? `重做：${nextRedo} (Ctrl+Shift+Z)` : "重做 (Ctrl+Shift+Z)"}
        >
          ↷ 重做
        </button>
      </div>

      <div className="toolbar__group">
        <button type="button" onClick={onLayout} title={`整理布局 (${keyHint("layout")})`}>
          整理
        </button>
        <button
          type="button"
          data-testid="drawer-toggle"
          onClick={() => useUiStore.getState().toggleDrawer()}
          title={`日志与诊断 (${keyHint("toggleDrawer")})`}
        >
          抽屉
        </button>
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

      <div className="toolbar__doc">
        <input
          className="toolbar__name"
          value={name ?? ""}
          spellCheck={false}
          placeholder="未命名"
          onChange={(e) => setName(e.target.value)}
        />
        {/* 脏标记：没有它用户不知道自己有没有存过（交互清单 P0 #13） */}
        {dirty && <span className="toolbar__dirty" title="有未保存的改动">●</span>}
        {filePath && (
          <span className="toolbar__path" title={filePath}>
            {baseName(filePath)}
          </span>
        )}
      </div>
    </header>
  );
}
