// 底部抽屉（m3-plan §2.5）：日志、诊断列表、缓存统计。
// 三样都是「跑完之后想看一眼」的东西，塞进右侧检查器会把参数挤没。

import { useEffect } from "react";

import { clearCache, formatBytes, refreshCacheStats, useCacheStore } from "../store/cache";
import { useExecutionStore } from "../store/execution";
import { useGraphStore } from "../store/graph";
import { useManifestStore } from "../store/manifest";
import { useUiStore, type DrawerTab } from "../store/ui";

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

function DiagnosticsTab() {
  const items = useDiagnostics();
  const label = useManifestStore((s) => s.operatorsById);
  const doc = useGraphStore((s) => s.doc);

  if (items.length === 0) {
    return <p className="drawer__empty">没有诊断。上一次运行是干净的。</p>;
  }
  return (
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
