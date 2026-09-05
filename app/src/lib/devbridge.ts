// 验收用的窗口桥：把 store 挂到 window.__lyflow，让 CDP 脚本既能点按钮也能读状态。
// 三条自律（只读转发、应用代码不许 import、正式构建里也在）见 app/README.md。

import { transport } from "../transport";
import { requestPlan, useCacheStore } from "../store/cache";
import { useExecutionStore } from "../store/execution";
import type { NodeState } from "../types/execution";
import { useGraphStore } from "../store/graph";
import { useManifestStore } from "../store/manifest";
import { useUiStore } from "../store/ui";

export interface StateTransition {
  nodeId: string;
  state: NodeState;
  at: number;
}

interface DevBridge {
  version: string;
  transport: typeof transport;
  stores: {
    graph: typeof useGraphStore;
    ui: typeof useUiStore;
    manifest: typeof useManifestStore;
    execution: typeof useExecutionStore;
    cache: typeof useCacheStore;
  };
  /** 立刻编译一次，不等 debounce。验收脚本不想为 150ms 睡一觉。 */
  plan(): Promise<void>;
  /** 节点状态变迁的流水账，用来断言「节点依次变色」。 */
  transitions: StateTransition[];
  clearTransitions(): void;
  /** 把 Map 拍平成可以 JSON 化的对象 —— CDP 的 evaluate 只认 JSON。 */
  snapshot(): unknown;
}

declare global {
  interface Window {
    __lyflow?: DevBridge;
  }
}

export function installDevBridge(): void {
  if (typeof window === "undefined" || window.__lyflow) return;

  const transitions: StateTransition[] = [];
  let previous = new Map<string, NodeState>();

  useExecutionStore.subscribe((state) => {
    const next = new Map<string, NodeState>();
    for (const [id, node] of state.nodes) {
      next.set(id, node.state);
      if (previous.get(id) !== node.state) {
        transitions.push({ nodeId: id, state: node.state, at: Date.now() });
      }
    }
    previous = next;
  });

  window.__lyflow = {
    version: "m3",
    transport,
    stores: {
      graph: useGraphStore,
      ui: useUiStore,
      manifest: useManifestStore,
      execution: useExecutionStore,
      cache: useCacheStore,
    },
    async plan() {
      const g = useGraphStore.getState();
      await requestPlan(g.doc, g.filePath);
    },
    transitions,
    clearTransitions() {
      transitions.length = 0;
      previous = new Map();
    },
    snapshot() {
      const g = useGraphStore.getState();
      const e = useExecutionStore.getState();
      const u = useUiStore.getState();
      const m = useManifestStore.getState();
      const c = useCacheStore.getState();
      return {
        transport: m.transportKind,
        manifestStatus: m.status,
        operatorCount: m.bundle?.operators.length ?? 0,
        generation: m.coreInfo?.generation ?? 0,
        doc: g.doc,
        filePath: g.filePath,
        dirty: g.dirty,
        undoLabel: g.past[g.past.length - 1]?.label ?? null,
        selected: [...u.selectedNodes],
        drawer: u.drawer,
        helpOpen: u.helpOpen,
        cache: {
          stats: c.stats,
          plan: Object.fromEntries([...c.plan].map(([id, n]) => [id, n])),
          ranWith: Object.fromEntries(c.ranWith),
          stale: [...c.plan]
            .filter(([id, n]) => {
              const previous = c.ranWith.get(id);
              return previous !== undefined && n.cacheKey !== previous && !n.cached;
            })
            .map(([id]) => id),
        },
        run: {
          runId: e.runId,
          status: e.runStatus,
          durationMs: e.durationMs,
          stale: e.stale,
          targets: e.targets,
          error: e.error,
          nodes: Object.fromEntries(
            [...e.nodes].map(([id, n]) => [
              id,
              {
                state: n.state,
                durationMs: n.durationMs ?? null,
                elementCount: n.stats?.elementCount ?? null,
                cached: n.stats?.cached === true,
                bypassed: n.stats?.bypassed === true,
                errors: n.errors,
              },
            ]),
          ),
          logs: e.logs.slice(-40),
        },
      };
    },
  };
}
