// 验收用的窗口桥：把 store 挂到 window.__lyflow，让 CDP 脚本既能点按钮也能读状态。
// 三条自律（只读转发、应用代码不许 import、正式构建里也在）见 app/README.md。

import { transport } from "../transport";
import { requestPlan, useCacheStore } from "../store/cache";
import {
  onNodeTransition,
  startRun,
  useExecutionStore,
  type RunRequest,
  type StateTransition,
} from "../store/execution";
import { useGraphStore } from "../store/graph";
import { useManifestStore } from "../store/manifest";
import { useUiStore } from "../store/ui";
import { levelOf, pathPrefix } from "./subgraph";

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
  /** 发起一次运行。走的是界面用的那条 startRun —— 直接调 transport 的话
   *  execution store 认不出 runId，事件会全进 orphans。 */
  run(request?: RunRequest): Promise<void>;
  /** 节点状态变迁的流水账，用来断言「节点依次变色」。
   *  来自**事件**而不是 store 快照 —— 16 ms 的合并窗口会把中间态吃掉。 */
  transitions: StateTransition[];
  clearTransitions(): void;
  /** 每次运行结束的时刻，live preview 的「跟手」断言靠它算延迟。 */
  runMarks: { runId: string; status: string; at: number }[];
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
  onNodeTransition((t) => transitions.push(t));

  const runMarks: { runId: string; status: string; at: number }[] = [];
  let lastStatus = "idle";
  useExecutionStore.subscribe((state) => {
    if (state.runStatus === lastStatus) return;
    lastStatus = state.runStatus;
    if (state.runStatus === "running" || state.runStatus === "idle") return;
    runMarks.push({
      runId: state.runId ?? "",
      status: state.runStatus,
      at: performance.now(),
    });
    if (runMarks.length > 50) runMarks.shift();
  });

  window.__lyflow = {
    version: "m4",
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
    async run(request) {
      const g = useGraphStore.getState();
      await startRun(g.doc, g.filePath, request ?? {});
    },
    transitions,
    clearTransitions() {
      transitions.length = 0;
      runMarks.length = 0;
    },
    runMarks,
    snapshot() {
      const g = useGraphStore.getState();
      const e = useExecutionStore.getState();
      const u = useUiStore.getState();
      const m = useManifestStore.getState();
      const c = useCacheStore.getState();
      const level = levelOf(g.doc, u.path);
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
        // 当前层级（F2）。验收脚本据此断言「进了子图看见的是哪几个节点」。
        path: u.path.map((p) => ({ ...p })),
        pathPrefix: pathPrefix(u.path),
        level: {
          nodes: level.nodes.map((n) => n.id),
          edges: level.edges.map((edge) => edge.id),
        },
        subgraphs: Object.fromEntries(
          Object.entries(g.doc.subgraphs ?? {}).map(([id, def]) => [
            id,
            {
              name: def.name ?? null,
              nodes: def.nodes.map((n) => n.id),
              edges: def.edges.length,
              inputs: def.inputs.map((i) => i.name),
              outputs: def.outputs.map((o) => o.name),
              params: def.params.map((p) => ({ name: p.name, binds: p.binds })),
            },
          ]),
        ),
        preview: {
          active: u.previewing,
          autoRun: u.autoRun,
          maxPoints: u.previewMaxPoints,
          lastRunWasPreview: e.preview,
        },
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
          preview: e.preview,
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
