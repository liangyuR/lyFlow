// 底部抽屉（m3-plan §2.5）：日志、诊断列表、缓存统计。
// 三样都是「跑完之后想看一眼」的东西，塞进右侧检查器会把参数挤没。

import { useCallback, useEffect, useRef, useState } from "react";

import { useDragFraction } from "../hooks/useDragFraction";
import { rootOf } from "../lib/root";

import { formatOutputValue, sortSummaryOutputs } from "../lib/outputs";
import { RunHistoryTab } from "./RunHistoryTab";
import { describeEventNode } from "../lib/subgraph";
import { clearCache, formatBytes, refreshCacheStats, useCacheStore } from "../store/cache";
import { useExecutionStore } from "../store/execution";
import { useGraphStore } from "../store/graph";
import { useManifestStore } from "../store/manifest";
import { useUiStore, type DrawerTab } from "../store/ui";
import type { OutputState, SummaryStatus } from "../types/execution";

/** 抽屉拖到多高（占整个编辑器的比例）记在这个键下。没拖过时是样式表的 220 px。 */
const DRAWER_FRACTION_KEY = "lyflow.drawer.fraction";

const TABS: { id: DrawerTab; label: string }[] = [
  { id: "log", label: "日志" },
  { id: "diagnostics", label: "诊断" },
  { id: "runs", label: "调参" },
  { id: "cache", label: "缓存" },
];

/** 全部节点的诊断拍平成一条条，点击定位到节点/参数。 */
function useDiagnostics() {
  const nodes = useExecutionStore((s) => s.nodes);
  const out: { nodeId: string; code: string; message: string; paramPath?: string | undefined }[] = [];
  for (const [nodeId, node] of nodes) {
    for (const e of node.errors) {
      out.push({ nodeId, code: e.code, message: e.message, paramPath: e.paramPath });
    }
  }
  return out;
}

/** 日志页。节点写名字（子图里带上它在哪几层，与诊断页同一个写法），点一下打开到它；
 *  一次运行几百条日志时能只看警告与错误、按节点或内容筛。 */
function LogTab() {
  const logs = useExecutionStore((s) => s.logs);
  const doc = useGraphStore((s) => s.doc);
  const ops = useManifestStore((s) => s.operatorsById);
  const [onlyWarn, setOnlyWarn] = useState(false);
  const [query, setQuery] = useState("");
  if (logs.length === 0) return <p className="drawer__empty">还没有日志。运行一次试试。</p>;

  const described = new Map<string, ReturnType<typeof describeEventNode>>();
  const describe = (id: string) => {
    let d = described.get(id);
    if (!d) {
      d = describeEventNode(doc, ops, id);
      described.set(id, d);
    }
    return d;
  };
  const q = query.trim().toLowerCase();
  const shown = logs.filter((l) => {
    if (onlyWarn && l.level !== "warn" && l.level !== "error") return false;
    if (!q) return true;
    const who = l.nodeId ? `${l.nodeId} ${describe(l.nodeId).names.join(" ")}` : "";
    return `${who} ${l.message}`.toLowerCase().includes(q);
  });
  return (
    <>
      <div className="drawer__logbar">
        <label>
          <input type="checkbox" checked={onlyWarn} onChange={(e) => setOnlyWarn(e.target.checked)} />
          只看警告与错误
        </label>
        <input
          type="search"
          value={query}
          placeholder="筛选：节点或内容"
          spellCheck={false}
          data-testid="drawer-log-filter"
          onChange={(e) => setQuery(e.target.value)}
        />
        <span className="drawer__logcount" data-testid="drawer-log-count">
          {shown.length} / {logs.length}
        </span>
      </div>
      <ol className="drawer__logs" data-testid="drawer-logs">
        {shown.map((l) => {
          const d = l.nodeId ? describe(l.nodeId) : null;
          return (
            <li key={l.seq} className={`drawer__log drawer__log--${l.level}`}>
              <span className="drawer__log-level">{l.level}</span>
              {l.nodeId && d && (
                <button
                  type="button"
                  className="drawer__log-node"
                  title={l.nodeId}
                  disabled={!d.reveal}
                  onClick={() => {
                    const r = d.reveal;
                    if (r) useUiStore.getState().revealNode(r.path, r.localId);
                  }}
                >
                  {d.names.join(" › ")}
                </button>
              )}
              <span className="drawer__log-text">{l.message}</span>
            </li>
          );
        })}
      </ol>
    </>
  );
}

const SUMMARY_STATUS_LABEL: Record<SummaryStatus, string> = {
  ok: "全部拿到",
  degraded: "有降级",
  failed: "失败",
};

const OUTPUT_STATE_LABEL: Record<OutputState, string> = {
  value: "有值",
  inactive: "本来就没有",
  failed: "本该有、崩了",
};

/** 诊断抽屉顶部的运行收尾（ADR-0022）。一行结论 + 每个图级输出的三态 + 全部决策。
 *  这一段整个来自 core 的 summary —— 前端一个字都不重建。图级输出写出读数与判定（以前只有「有值」与
 *  元素数，读数要一个个点开节点看），有问题的排在上面，节点写名字、点一下打开到它。 */
function SummaryHeader() {
  const summary = useExecutionStore((s) => s.summary);
  const doc = useGraphStore((s) => s.doc);
  const ops = useManifestStore((s) => s.operatorsById);
  if (!summary) return null;

  const outputs = sortSummaryOutputs(Object.entries(summary.outputs));
  const decisions = Object.entries(summary.decisions);

  return (
    <div className="drawer__summary" data-testid="drawer-summary" data-status={summary.status}>
      <p className={`drawer__summary-status drawer__summary-status--${summary.status}`}>
        <span data-testid="summary-status">
          {SUMMARY_STATUS_LABEL[summary.status] ?? summary.status}
        </span>
        <code>{summary.status}</code>
        {typeof summary.durationMs === "number" && (
          <span className="drawer__summary-duration">{summary.durationMs.toFixed(0)} ms</span>
        )}
      </p>

      {outputs.length > 0 && (
        <ul className="drawer__summary-outputs" data-testid="summary-outputs">
          {outputs.map(([name, o]) => {
            const where = describeEventNode(doc, ops, o.node);
            const verdict = o.value?.kind === "Measurement" ? o.value.verdict || null : null;
            // Record（gap 的结果包这类）展开是几 KB 的 JSON：收尾里照旧只写个数，内容在检查器里看
            const text = o.state === "value" && o.value && o.value.kind !== "Record" ? formatOutputValue(o) : null;
            return (
              <li key={name} data-testid={`summary-output-${name}`} data-state={o.state} data-verdict={verdict ?? undefined}>
                <span className="drawer__summary-name">{name}</span>
                <code className={`drawer__summary-state drawer__summary-state--${o.state}`}>
                  {OUTPUT_STATE_LABEL[o.state] ?? o.state}
                </code>
                {text && (
                  <span className="drawer__summary-value" data-testid={`summary-value-${name}`} title={text}>
                    {text}
                  </span>
                )}
                {verdict && <span className={`insp-out__verdict is-${verdict}`}>{verdict}</span>}
                <button
                  type="button"
                  className="drawer__log-node drawer__summary-node"
                  data-testid={`summary-node-${name}`}
                  title={`${o.node}.${o.port}${where.reveal ? " —— 点此打开到它" : ""}`}
                  disabled={!where.reveal}
                  onClick={() => {
                    const r = where.reveal;
                    if (r) useUiStore.getState().revealNode(r.path, r.localId);
                  }}
                >
                  {where.names.join(" › ")}.{o.port}
                </button>
                {/* failed 的那一维带着回溯出来的源头：不必再去事件流里找 */}
                {o.state === "failed" && (
                  <span className="drawer__summary-from">
                    来自 {o.from} · {o.code}
                  </span>
                )}
                {o.state === "inactive" && o.reason && (
                  <span className="drawer__summary-from">{o.reason}</span>
                )}
                {o.state === "value" && !text && typeof o.elementCount === "number" && (
                  <span className="drawer__summary-from">{o.elementCount} 个</span>
                )}
              </li>
            );
          })}
        </ul>
      )}

      {decisions.length > 0 && (
        <ul className="drawer__summary-decisions" data-testid="summary-decisions">
          {decisions.map(([nodeId, d]) => (
            <li key={nodeId} data-testid={`summary-decision-${nodeId}`}>
              <span className="drawer__summary-name">{nodeId}</span>
              <code className="drawer__summary-choice">{d.choice ?? "?"}</code>
              {d.reason && <span className="drawer__summary-from">{d.reason}</span>}
            </li>
          ))}
        </ul>
      )}
    </div>
  );
}

function DiagnosticsTab() {
  const items = useDiagnostics();
  const label = useManifestStore((s) => s.operatorsById);
  const doc = useGraphStore((s) => s.doc);
  const summary = useExecutionStore((s) => s.summary);

  if (items.length === 0) {
    return (
      <>
        <SummaryHeader />
        <p className="drawer__empty">
          {summary ? "没有节点级诊断。" : "没有诊断。上一次运行是干净的。"}
        </p>
      </>
    );
  }
  return (
    <>
    <SummaryHeader />
    <ul className="drawer__diags" data-testid="drawer-diagnostics">
      {items.map((d, i) => {
        // 事件 id 是路径（ADR-0010）：子图里的节点带上一层层的子图名，点了打开到那一层
        const target = describeEventNode(doc, label, d.nodeId);
        return (
          <li key={`${d.nodeId}-${i}`}>
            <button
              type="button"
              data-testid={`diag-${d.nodeId}`}
              onClick={() => {
                const r = target.reveal;
                if (r) useUiStore.getState().revealNode(r.path, r.localId, r.exact ? d.paramPath : undefined);
                else useUiStore.getState().focusDiagnostic(d.nodeId, d.paramPath);
              }}
            >
              <span className="drawer__diag-node">{target.names.join(" › ")}</span>
              <code className="drawer__diag-code">{d.code}</code>
              {d.paramPath && <code className="drawer__diag-param">{d.paramPath}</code>}
              <span className="drawer__diag-text">{d.message}</span>
            </button>
          </li>
        );
      })}
    </ul>
    </>
  );
}

function CacheTab() {
  const stats = useCacheStore((s) => s.stats);
  const plan = useCacheStore((s) => s.plan);

  useEffect(() => {
    void refreshCacheStats();
  }, []);

  let cached = 0;
  for (const n of plan.values()) if (n.cached) cached += 1;

  return (
    <div className="drawer__cache" data-testid="drawer-cache">
      {stats ? (
        <dl>
          <div><dt>条目</dt><dd data-testid="cache-entries">{stats.entries}</dd></div>
          <div><dt>占用</dt><dd data-testid="cache-bytes">{formatBytes(stats.bytes)}</dd></div>
          <div><dt>预算</dt><dd>{formatBytes(stats.budgetBytes)}</dd></div>
          <div><dt>命中 / 未命中</dt><dd>{stats.hits} / {stats.misses}</dd></div>
          <div><dt>淘汰</dt><dd>{stats.evictions}</dd></div>
          <div><dt>本图已缓存</dt><dd>{cached} / {plan.size}</dd></div>
        </dl>
      ) : (
        <p className="drawer__empty">读不到缓存统计（浏览器模式没有 core）。</p>
      )}
      <div className="drawer__cache-actions">
        <button type="button" onClick={() => void refreshCacheStats()}>刷新</button>
        <button
          type="button"
          data-testid="cache-clear"
          onClick={() => {
            void clearCache().then(() => useUiStore.getState().showToast("缓存已清空"));
          }}
        >
          清空缓存
        </button>
      </div>
    </div>
  );
}

export function BottomDrawer() {
  const drawer = useUiStore((s) => s.drawer);
  const toggle = useUiStore((s) => s.toggleDrawer);
  const logCount = useExecutionStore((s) => s.logs.length);
  const diagCount = useDiagnostics().length;
  // 上沿可以往上拖高（日志、诊断一多，220 px 只看得到十来行）
  const self = useRef<HTMLDivElement>(null);
  const getRoot = useCallback(() => rootOf(self.current), []);
  const getSelf = useCallback(() => self.current, []);
  const resize = useDragFraction({
    column: getRoot, pane: getSelf, edge: "bottom", minPx: 120, restMinPx: 300, persistKey: DRAWER_FRACTION_KEY,
  });

  return (
    <div
      className={`drawer${drawer ? " is-open" : ""}`}
      ref={self}
      style={drawer && resize.fraction !== null ? { height: `${resize.fraction * 100}%` } : undefined}
      data-testid="drawer"
      data-tab={drawer ?? ""}
    >
      {drawer && (
        <div
          className="drawer__resize"
          onPointerDown={resize.onPointerDown}
          onDoubleClick={resize.reset}
          role="separator"
          aria-orientation="horizontal"
          title="拖动调整高度，双击恢复默认"
          data-testid="drawer-splitter"
        />
      )}
      <div className="drawer__tabs">
        {TABS.map((t) => (
          <button
            key={t.id}
            type="button"
            className={drawer === t.id ? "is-active" : ""}
            data-testid={`drawer-tab-${t.id}`}
            onClick={() => toggle(t.id)}
          >
            {t.label}
            {t.id === "log" && logCount > 0 && <span className="drawer__badge">{logCount}</span>}
            {t.id === "diagnostics" && diagCount > 0 && (
              <span className="drawer__badge drawer__badge--error">{diagCount}</span>
            )}
          </button>
        ))}
        <span className="drawer__spacer" />
        {drawer && (
          <button type="button" className="drawer__close" onClick={() => toggle(drawer)}>
            收起
          </button>
        )}
      </div>
      {drawer && (
        <div className="drawer__body">
          {drawer === "log" && <LogTab />}
          {drawer === "diagnostics" && <DiagnosticsTab />}
          {drawer === "runs" && <RunHistoryTab />}
          {drawer === "cache" && <CacheTab />}
        </div>
      )}
    </div>
  );
}
