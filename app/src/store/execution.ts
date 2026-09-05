// 执行状态 store。运行时状态，不进 GraphDoc、不进撤销栈（ADR-0002）。
// runId 对不上的事件先攒进 orphans 等 beginRun 认领；seq 不连续只 warn 不停摆。

import { useMemo } from "react";
import { create } from "zustand";

import { transport } from "../transport";
import type {
  Diagnostic,
  ExecutionEvent,
  NodeState,
  NodeStats,
  RunStatus,
} from "../types/execution";
import type { GraphDoc } from "../types/graph";

// 可选字段一律写成 `?: T | undefined`：tsconfig 开了 exactOptionalPropertyTypes，
// 展开赋值时那些字段的类型本来就是 T | undefined。
export interface NodeExecution {
  state: NodeState;
  durationMs?: number | undefined;
  progress?: number | undefined;
  message?: string | undefined;
  errors: Diagnostic[];
  stats?: NodeStats | undefined;
}

export interface LogEntry {
  seq: number;
  level: "debug" | "info" | "warn" | "error";
  nodeId?: string | undefined;
  message: string;
}

/** 日志环形缓冲的容量。一次运行几万条进度日志是常态，全留着会把内存吃光。 */
const LOG_LIMIT = 500;

/** 未认领事件的上限。正常情况只会攒到个位数（就是 run_started 前后那几条）。 */
const ORPHAN_LIMIT = 2000;

export type RunPhase = "idle" | "running" | "ok" | "error" | "cancelled";

interface ExecutionState {
  runId: string | null;
  runStatus: RunPhase;
  startedAt: number | null;
  durationMs: number | null;
  /** 本次运行是「只跑到某个节点」还是全图。空 = 全图。 */
  targets: string[];
  nodes: Map<string, NodeExecution>;
  logs: LogEntry[];
  /** 结果是否已经过时：运行之后图被改过（交互清单 P1 #23）。
   *  否则用户看着一片绿，却不知道那是三次修改之前的结果。 */
  stale: boolean;
  /** 上一次收到的 seq，用来检测丢包。 */
  lastSeq: number;
  /** 启动失败之类的错误，直接显示在工具栏上。 */
  error: string | null;
  /** 还不知道该归给谁的事件：C++ 先起线程再返回句柄，事件可能比 `run_graph`
   *  的返回值先到。直接丢会让小图整场跑完而界面毫无反应，所以先攒着认领。 */
  orphans: ExecutionEvent[];

  beginRun(runId: string, targets: string[]): void;
  failRun(message: string): void;
  apply(event: ExecutionEvent): void;
  markStale(): void;
  reset(): void;
}

const emptyNode = (): NodeExecution => ({ state: "idle", errors: [] });

export const useExecutionStore = create<ExecutionState>((set, get) => ({
  runId: null,
  runStatus: "idle",
  startedAt: null,
  durationMs: null,
  targets: [],
  nodes: new Map(),
  logs: [],
  stale: false,
  lastSeq: -1,
  error: null,
  orphans: [],

  beginRun(runId, targets) {
    const claimed = get()
      .orphans.filter((e) => e.runId === runId)
      .sort((a, b) => a.seq - b.seq);
    set({
      runId,
      runStatus: "running",
      startedAt: Date.now(),
      durationMs: null,
      targets,
      nodes: new Map(),
      logs: [],
      stale: false,
      lastSeq: -1,
      error: null,
      orphans: [],
    });
    // 认领在 run_graph 返回之前就到达的事件。
    for (const e of claimed) get().apply(e);
  },

  failRun(message) {
    set({ runId: null, runStatus: "error", error: message, durationMs: null, orphans: [] });
  },

  apply(event) {
    const s = get();
    if (s.runId !== event.runId) {
      // 还没认领的运行先攒着（上限防止失控的运行吃光内存）；已过期的 runId
      // 不会再被认领，攒下的那点很快被下一次 beginRun 清掉。
      if (s.orphans.length < ORPHAN_LIMIT) {
        set({ orphans: s.orphans.concat(event) });
      }
      return;
    }

    if (s.lastSeq >= 0 && event.seq !== s.lastSeq + 1) {
      console.warn(
        `[lyflow] 执行事件 seq 不连续：期望 ${s.lastSeq + 1}，收到 ${event.seq}。` +
          `界面继续更新，但可能漏了状态。`,
      );
    }

    const nodes = new Map(s.nodes);
    const patch: Partial<ExecutionState> = { lastSeq: event.seq };

    switch (event.kind) {
      case "run_started": {
        for (const id of event.plan ?? []) nodes.set(id, emptyNode());
        patch.nodes = nodes;
        patch.targets = event.targets ?? [];
        break;
      }
      case "node_state": {
        const prev = nodes.get(event.nodeId) ?? emptyNode();
        nodes.set(event.nodeId, {
          ...prev,
          state: event.state,
          durationMs: event.durationMs ?? prev.durationMs,
          stats: event.stats ?? prev.stats,
          // errors 用全量覆盖而不是追加：同一个节点在一次运行里只会失败一次，
          // 追加只会在重跑时留下上一轮的幽灵红框。
          errors: event.errors ?? (event.error ? [event.error] : []),
          // 进入 running 时清掉上一轮的进度，否则进度条会从 100% 开始倒着走
          progress: event.state === "running" ? 0 : prev.progress,
        });
        patch.nodes = nodes;
        break;
      }
      case "node_progress": {
        const prev = nodes.get(event.nodeId) ?? emptyNode();
        nodes.set(event.nodeId, {
          ...prev,
          progress: event.progress,
          message: event.message,
        });
        patch.nodes = nodes;
        break;
      }
      case "run_finished": {
        patch.runStatus = event.status as RunStatus;
        patch.durationMs = event.durationMs ?? null;
        break;
      }
      case "log": {
        const logs = s.logs.concat({
          seq: event.seq,
          level: event.level,
          nodeId: event.nodeId,
          message: event.message,
        });
        patch.logs = logs.length > LOG_LIMIT ? logs.slice(-LOG_LIMIT) : logs;
        break;
      }
    }

    set(patch);
  },

  markStale() {
    // 没跑过就无所谓过期。runStatus idle 时置 stale 会让节点无故变淡。
    if (get().runStatus === "idle" || get().stale) return;
    set({ stale: true });
  },

  reset() {
    set({
      runId: null,
      runStatus: "idle",
      startedAt: null,
      durationMs: null,
      targets: [],
      nodes: new Map(),
      logs: [],
      stale: false,
      lastSeq: -1,
      error: null,
      orphans: [],
    });
  },
}));

// 派生选择器 -----------------------------------------------------------------

const NO_ERRORS: ReadonlyMap<string, string> = new Map();

/** 某节点的执行状态。节点组件和检查器都按 id 现查。 */
export function useNodeExecution(nodeId: string): NodeExecution | undefined {
  return useExecutionStore((s) => s.nodes.get(nodeId));
}

/** 某节点的 paramPath → 错误消息，ParamControls 据此画红框（交互清单 P0 #15）。
 *  没有错误时返回同一个空 Map：新引用会让整个检查器每来一条事件就全量重渲染。 */
export function useParamErrors(nodeId: string): ReadonlyMap<string, string> {
  const node = useNodeExecution(nodeId);
  return useMemo(() => {
    if (!node || node.errors.length === 0) return NO_ERRORS;
    const out = new Map<string, string>();
    for (const e of node.errors) {
      if (e.paramPath) out.set(e.paramPath, e.message);
    }
    return out.size === 0 ? NO_ERRORS : out;
  }, [node]);
}

/** 「done 7 / error 1」那一行。 */
export function summarize(nodes: ReadonlyMap<string, NodeExecution>): {
  done: number;
  error: number;
  cancelled: number;
  running: number;
  total: number;
} {
  let done = 0;
  let error = 0;
  let cancelled = 0;
  let running = 0;
  for (const n of nodes.values()) {
    if (n.state === "done" || n.state === "skipped") done += 1;
    else if (n.state === "error") error += 1;
    else if (n.state === "cancelled") cancelled += 1;
    else if (n.state === "running") running += 1;
  }
  return { done, error, cancelled, running, total: nodes.size };
}

// 事件订阅 -------------------------------------------------------------------

/** 订阅只能有一份，守卫必须是**这个 Promise** 而不是它的结果：unlisten 在
 *  await 之后才赋值，StrictMode 的两遍 effect 都看到 null（见 README）。 */
let subscription: Promise<() => void> | null = null;

/** 在 App 挂载时调一次。重复调用是幂等的。 */
export async function subscribeExecutionEvents(): Promise<void> {
  subscription ??= transport.onExecutionEvent((e) => useExecutionStore.getState().apply(e));
  await subscription;
}

/** 启动一次运行。beginRun 只能在拿到 runId 之后，而事件可能更早到，
 *  所以认不出 runId 的先进 orphans、beginRun 时认领（见上）。 */
/** 本地的运行序号：两次 run_graph 走不同的 Tauri 工作线程，回复顺序不保证等于
 *  发起顺序。序号让后发的那次赢，与 C++ 侧「后开始的抢占先开始的」一致。 */
let runTicket = 0;

export async function startRun(
  doc: GraphDoc,
  graphPath: string | null,
  targets?: string[],
): Promise<void> {
  const store = useExecutionStore.getState();
  const ticket = ++runTicket;
  try {
    const runId = await transport.runGraph(doc, graphPath, targets);
    if (ticket !== runTicket) return;  // 已经有更晚的一次运行发起了，这次的回复作废
    store.beginRun(runId, targets ?? []);
  } catch (e) {
    if (ticket !== runTicket) throw e;
    store.failRun(e instanceof Error ? e.message : String(e));
    throw e;
  }
}

export async function cancelCurrentRun(): Promise<void> {
  const { runId, runStatus } = useExecutionStore.getState();
  if (!runId || runStatus !== "running") return;
  await transport.cancelRun(runId);
}
