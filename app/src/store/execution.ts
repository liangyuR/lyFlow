//
// 执行状态 store。
//
// **不进 GraphDoc，不进撤销栈。** 节点跑成什么样是运行时状态，和 selected、
// 画布视口是同一类东西（ADR-0002）。混进 GraphDoc 的后果是文件里出现
// "state": "done" 这种字段，以及按 Ctrl+Z 撤销掉一次运行结果。
//
// 事件流的两条纪律：
//   1. runId 对不上的事件不进状态 —— 抢占式运行（D3）下，旧 run 的事件会在
//      新 run 开始之后才到，混进去的话节点会在两次运行的状态之间跳。
//      但也不能直接扔：新 run 的事件可能比 run_graph 的返回值还早到，
//      所以先攒在 orphans 里，beginRun 时按 runId 认领（见下）。
//   2. seq 不连续就 warn 但继续 —— 丢一条事件不该让界面停摆，但必须留下痕迹，
//      否则「偶尔有个节点一直是 running」这种问题永远查不出来。
//

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
  /**
   * 结果是否已经过时：运行之后图被改过。
   * 缓存语义的可视化（交互清单 P1 #23），成本极低但很值钱 ——
   * 否则用户看着一片绿色的节点，却不知道那是三次修改之前的结果。
   */
  stale: boolean;
  /** 上一次收到的 seq，用来检测丢包。 */
  lastSeq: number;
  /** 启动失败之类的错误，直接显示在工具栏上。 */
  error: string | null;
  /**
   * 还不知道该归给谁的事件。
   *
   * C++ 的 `lyflow_run_start` 是**先起线程再返回句柄**，所以事件完全可能在
   * `run_graph` 这个 IPC 调用返回之前就到了前端 —— 那一刻 store 里还没有
   * runId。直接按「runId 对不上就丢」处理的话，一张小图可能整场运行都跑完了，
   * 界面上什么都没发生。所以先攒着，`beginRun` 时按 runId 认领。
   */
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
      // 还没认领的运行：先攒着（上限防止一场失控的运行把内存吃光）。
      // 已经过期的运行：runId 不会再被 beginRun 认领，攒下的那点很快被下一次
      // beginRun 清掉。
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

// ---------------------------------------------------------------------------
// 派生选择器
// ---------------------------------------------------------------------------

const NO_ERRORS: ReadonlyMap<string, string> = new Map();

/** 某节点的执行状态。节点组件和检查器都按 id 现查。 */
export function useNodeExecution(nodeId: string): NodeExecution | undefined {
  return useExecutionStore((s) => s.nodes.get(nodeId));
}

/**
 * 某节点的 paramPath → 错误消息。ParamControls 据此画红框（交互清单 P0 #15）。
 *
 * 没有错误时返回同一个空 Map 而不是每次新建：这个 hook 挂在每一个参数控件上，
 * 每次返回新引用会让整个检查器在每条事件到达时全量重渲染。
 */
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

// ---------------------------------------------------------------------------
// 事件订阅
// ---------------------------------------------------------------------------

/**
 * 订阅只能有一份，而守卫必须是**这个 Promise**，不是它的结果。
 *
 * 守 `unlisten !== null` 是不够的：它在 await 之后才赋值，而 StrictMode 的
 * 挂载→卸载→再挂载会让 effect 跑两遍，两次都看到 null。后果是注册了两个监听器，
 * 第一个的 unlisten 被覆盖再也调不到（泄漏），而且每条事件都被 apply 两遍 ——
 * 表现是控制台刷满「seq 不连续」的告警、日志条目翻倍。
 */
let subscription: Promise<() => void> | null = null;

/** 在 App 挂载时调一次。重复调用是幂等的。 */
export async function subscribeExecutionEvents(): Promise<void> {
  subscription ??= transport.onExecutionEvent((e) => useExecutionStore.getState().apply(e));
  await subscription;
}

/**
 * 启动一次运行。
 *
 * 「先 beginRun 再 await」不行 —— 那时还没有 runId；「先 await 再 beginRun」
 * 也不行 —— 事件可能在 invoke 返回之前就到了。所以走第三条路：
 * 认不出 runId 的事件先进 orphans，beginRun 时按 runId 认领（见上）。
 * 抢占式运行下这也顺带处理了「新 run 的头几条事件和旧 run 的尾巴交错」。
 */
/**
 * 本地的运行序号。
 *
 * 两次 run_graph 走的是不同的 Tauri 工作线程，**回复的顺序不保证等于运行开始的
 * 顺序**。用户快点两下（也就是 D3 抢占的正常路径），如果 A 的回复后到，
 * `beginRun(A)` 会盖掉 `beginRun(B)`，于是 store 认的是已经被取消的 A，
 * B 的事件全部落进 orphans 再也无人认领 —— 界面会永远停在 running。
 * 序号让后发的那次调用赢，与 C++ 侧「后开始的 run 抢占先开始的」一致。
 */
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
