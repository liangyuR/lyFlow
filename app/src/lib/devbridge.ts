//
// 验收用的窗口桥。
//
// 为什么需要它：本项目**不写 UI 单元测试**（CLAUDE.md），验收方式是用 CDP
// 驱动真实运行的 Tauri app（M1 的做法，抓到了三个单元测试发现不了的真 bug）。
// 但光靠 DOM 断言只能验到表象 —— 「节点变绿了」和「执行状态真的是 done」
// 是两回事，中间隔着整条事件流。所以把 store 显式挂到 window 上，
// 让验收脚本既能点按钮、也能读状态。
//
// 三条自律：
//   1. **只读 + 转发，不放业务逻辑。** 这里出现任何应用逻辑，就意味着验收
//      验的是这个文件而不是真实代码路径。
//   2. 应用代码一律不许 import 它。唯一的引用点是 main.tsx 的一行安装调用。
//   3. 它在正式构建里也在。这是一个内部工具，`window.__lyflow` 的存在不构成
//      风险；反过来，只在 dev 里有的话，就没法用它验证**安装包**能不能跑通，
//      而那恰恰是 M2 验收里最容易出问题的一条。
//

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
