// 给 `pnpm e2e:http` 用的窗口桥。与 app/src/devbridge.ts 同名同形，
// 但只暴露 HTTP 子集验收要用到的那些（app 那一份仍然是 Tauri 验收的权威）。

import {
  onNodeTransition,
  peekSourceOf,
  requestPlan,
  startRun,
  useCacheStore,
  useExecutionStore,
  useGraphStore,
  useManifestStore,
  usePeekStore,
  useUiStore,
  type RunRequest,
  type StateTransition,
  type Transport,
} from "@lyflow/editor";

interface HostBridge {
  version: string;
  transport: Transport;
  stores: {
    graph: typeof useGraphStore;
    ui: typeof useUiStore;
    manifest: typeof useManifestStore;
    execution: typeof useExecutionStore;
    cache: typeof useCacheStore;
    peek: typeof usePeekStore;
  };
  plan(): Promise<void>;
  run(request?: RunRequest): Promise<void>;
  transitions: StateTransition[];
  clearTransitions(): void;
  runOutputs(runId: string): Promise<unknown>;
  snapshot(): unknown;
}

declare global {
  interface Window {
    __lyflow?: HostBridge;
  }
}

export function installHostBridge(transport: Transport): void {
  if (typeof window === "undefined" || window.__lyflow) return;

  const transitions: StateTransition[] = [];
  onNodeTransition((t) => transitions.push(t));

  window.__lyflow = {
    version: "host-react",
    transport,
    stores: {
      graph: useGraphStore,
      ui: useUiStore,
      manifest: useManifestStore,
      execution: useExecutionStore,
      cache: useCacheStore,
      peek: usePeekStore,
    },
    async plan() {
      const g = useGraphStore.getState();
      await requestPlan(g.doc, g.filePath);
    },
    async run(request) {
      const g = useGraphStore.getState();
      await startRun(g.doc, g.filePath, request ?? {});
    },
    transitions,
    clearTransitions() {
      transitions.length = 0;
    },
    async runOutputs(runId: string) {
      return transport.getRunOutputs(runId);
    },
    snapshot() {
      const g = useGraphStore.getState();
      const e = useExecutionStore.getState();
      const u = useUiStore.getState();
      const m = useManifestStore.getState();
      const p = usePeekStore.getState();
      return {
        transport: m.transportKind,
        manifestStatus: m.status,
        operatorCount: m.bundle?.operators.length ?? 0,
        doc: g.doc,
        filePath: g.filePath,
        dirty: g.dirty,
        selected: [...u.selectedNodes],
        peek: p.windows.map((w) => {
          const src = peekSourceOf(g.doc, w.path, w.from);
          return {
            id: w.id,
            edgeId: w.edgeId,
            view: w.view,
            locked: !!w.locked,
            node: w.from.node,
            port: w.from.port,
            type: src.type,
            status: src.status,
          };
        }),
        run: {
          runId: e.runId,
          status: e.runStatus,
          durationMs: e.durationMs,
          stale: e.stale,
          targets: e.targets,
          outputs: e.outputs,
          error: e.error,
          nodes: Object.fromEntries(
            [...e.nodes].map(([id, n]) => [
              id,
              {
                state: n.state,
                elementCount: n.stats?.elementCount ?? null,
                reason: n.stats?.reason ?? null,
                errors: n.errors,
              },
            ]),
          ),
        },
      };
    },
  };
}
