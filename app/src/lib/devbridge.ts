// 验收用的窗口桥：把 store 挂到 window.__lyflow，让 CDP 脚本既能点按钮也能读状态。
// 三条自律（只读转发、应用代码不许 import、正式构建里也在）见 app/README.md。

import { transport } from "../transport";
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
  };
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
    version: "m2",
    transport,
    stores: {
      graph: useGraphStore,
      ui: useUiStore,
      manifest: useManifestStore,
      execution: useExecutionStore,
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
      return {
        transport: m.transportKind,
        manifestStatus: m.status,
        operatorCount: m.bundle?.operators.length ?? 0,
        doc: g.doc,
        filePath: g.filePath,
        dirty: g.dirty,
        selected: [...u.selectedNodes],
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
