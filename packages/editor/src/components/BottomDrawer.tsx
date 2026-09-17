// 底部抽屉（m3-plan §2.5）：日志、诊断列表、缓存统计。
// 三样都是「跑完之后想看一眼」的东西，塞进右侧检查器会把参数挤没。

import { useEffect } from "react";

import { clearCache, formatBytes, refreshCacheStats, useCacheStore } from "../store/cache";
import { useExecutionStore } from "../store/execution";
import { useGraphStore } from "../store/graph";
import { useManifestStore } from "../store/manifest";
import { useUiStore, type DrawerTab } from "../store/ui";
import type { OutputState, SummaryStatus } from "../types/execution";

const TABS: { id: DrawerTab; label: string }[] = [
  { id: "log", label: "日志" },
  { id: "diagnostics", label: "诊断" },
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

function LogTab() {
  const logs = useExecutionStore((s) => s.logs);
  if (logs.length === 0) return <p className="drawer__empty">还没有日志。运行一次试试。</p>;
  return (
    <ol className="drawer__logs" data-testid="drawer-logs">
      {logs.map((l) => (
        <li key={l.seq} className={`drawer__log drawer__log--${l.level}`}>
          <span className="drawer__log-level">{l.level}</span>
          {l.nodeId && <span className="drawer__log-node">{l.nodeId}</span>}
          <span className="drawer__log-text">{l.message}</span>
        </li>
      ))}
    </ol>
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
 *  这一段整个来自 core 的 summary —— 前端一个字都不重建。 */
function SummaryHeader() {
  const summary = useExecutionStore((s) => s.summary);
  if (!summary) return null;

  const outputs = Object.entries(summary.outputs);
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
          {outputs.map(([name, o]) => (
            <li key={name} data-testid={`summary-output-${name}`} data-state={o.state}>
              <span className="drawer__summary-name">{name}</span>
              <code className={`drawer__summary-state drawer__summary-state--${o.state}`}>
                {OUTPUT_STATE_LABEL[o.state] ?? o.state}
              </code>
              <span className="drawer__summary-where">
                {o.node}.{o.port}
              </span>
              {/* failed 的那一维带着回溯出来的源头：不必再去事件流里找 */}
              {o.state === "failed" && (
                <span className="drawer__summary-from">
                  来自 {o.from} · {o.code}
                </span>
              )}
              {o.state === "inactive" && o.reason && (
                <span className="drawer__summary-from">{o.reason}</span>
              )}
              {o.state === "value" && typeof o.elementCount === "number" && (
                <span className="drawer__summary-from">{o.elementCount} 个</span>
              )}
            </li>
          ))}
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
        const node = doc.nodes.find((n) => n.id === d.nodeId);
        const op = node ? label.get(node.op) : undefined;
        return (
          <li key={`${d.nodeId}-${i}`}>
            <button
              type="button"
              data-testid={`diag-${d.nodeId}`}
              onClick={() => useUiStore.getState().focusDiagnostic(d.nodeId, d.paramPath)}
            >
              <span className="drawer__diag-node">{node?.ui?.title ?? op?.label ?? d.nodeId}</span>
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

  return (
    <div className={`drawer${drawer ? " is-open" : ""}`} data-testid="drawer" data-tab={drawer ?? ""}>
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
          {drawer === "cache" && <CacheTab />}
        </div>
      )}
    </div>
  );
}
