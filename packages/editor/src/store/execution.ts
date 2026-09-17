// 执行状态 store。运行时状态不进 GraphDoc、不进撤销栈；runId 对不上的事件先攒进 orphans。
// M4：node_state / node_progress 按 16 ms 批量落库，事件 id 是路径、按层聚合（F2）。

import { create } from "zustand";

import { pathPrefix, type SubPath } from "../lib/subgraph";
import { transport } from "../transport";
import { refreshCacheStats, useCacheStore } from "./cache";
import { useUiStore } from "./ui";
import type {
  Diagnostic,
  ExecutionEvent,
  GraphOutputRef,
  NodeState,
  NodeStats,
  RunStatus,
  RunSummary,
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
  /** 聚合出来的（子图节点）：内部一共几个节点、跑完了几个。 */
  children?: { total: number; finished: number } | undefined;
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

/** 事件合并窗口（§4）。一帧一次 set，三百个节点的图才不会每条事件重渲一遍。 */
const BATCH_MS = 16;

export type RunPhase = "idle" | "running" | "ok" | "error" | "cancelled";

interface ExecutionState {
  runId: string | null;
  runStatus: RunPhase;
  /** 本次运行是不是预览（ADR-0011）。正式结果到达后会覆盖它。 */
  preview: boolean;
  startedAt: number | null;
  durationMs: number | null;
  /** 本次运行是「只跑到某个节点」还是全图。空 = 全图。 */
  targets: string[];
  /** 图级命名输出的声明（ADR-0017）。run_started 带过来，前端不再自己解析图。 */
  outputs: GraphOutputRef[];
  /** core 产出的运行收尾（ADR-0022）。run_finished 带过来，前端只显示不重建。
   *  老 core（ABI < v9）不带它，诊断抽屉顶部那一段就不显示。 */
  summary: RunSummary | null;
  /** 事件里的 nodeId 是**路径**，键就是路径原样。按层聚合见 useNodeExecution。 */
  nodes: Map<string, NodeExecution>;
  logs: LogEntry[];
  /** 结果是否已经过时：运行之后图被改过（交互清单 P1 #23）。 */
  stale: boolean;
  /** 上一次收到的 seq，用来检测丢包。 */
  lastSeq: number;
  /** 启动失败之类的错误，直接显示在工具栏上。 */
  error: string | null;
  /** 还不知道该归给谁的事件：C++ 先起线程再返回句柄，事件可能比 `run_graph`
   *  的返回值先到。直接丢会让小图整场跑完而界面毫无反应，所以先攒着认领。 */
  orphans: ExecutionEvent[];

  beginRun(runId: string, targets: string[], preview: boolean): void;
  failRun(message: string): void;
  apply(event: ExecutionEvent): void;
  markStale(): void;
  reset(): void;
}

const emptyNode = (): NodeExecution => ({ state: "idle", errors: [] });

// -------------------------------------------------------------- 事件合并

/** 攒着还没落库的节点状态。null = 没有待落库的改动。 */
let staged: Map<string, NodeExecution> | null = null;
let flushTimer: ReturnType<typeof setTimeout> | null = null;
let latestSeq = -1;

/** 事件级的状态流水账。devbridge 靠它断言「节点依次变色」——
 *  从 store 快照推的话，合并窗口会把中间态吃掉。 */
export interface StateTransition {
  nodeId: string;
  state: NodeState;
  at: number;
}
const transitionListeners = new Set<(t: StateTransition) => void>();

export function onNodeTransition(fn: (t: StateTransition) => void): () => void {
  transitionListeners.add(fn);
  return () => transitionListeners.delete(fn);
}

function flushNow(): void {
  if (flushTimer !== null) {
    clearTimeout(flushTimer);
    flushTimer = null;
  }
  if (!staged) return;
  const nodes = staged;
  staged = null;
  useExecutionStore.setState({ nodes, lastSeq: latestSeq });
}

function scheduleFlush(): void {
  if (flushTimer !== null) return;
  flushTimer = setTimeout(() => {
    flushTimer = null;
    flushNow();
  }, BATCH_MS);
}

function stage(): Map<string, NodeExecution> {
  staged ??= new Map(useExecutionStore.getState().nodes);
  return staged;
}

function dropStaged(): void {
  if (flushTimer !== null) {
    clearTimeout(flushTimer);
    flushTimer = null;
  }
  staged = null;
  latestSeq = -1;
}

export const useExecutionStore = create<ExecutionState>((set, get) => ({
  runId: null,
  runStatus: "idle",
  preview: false,
  startedAt: null,
  durationMs: null,
  targets: [],
  outputs: [],
  summary: null,
  nodes: new Map(),
  logs: [],
  stale: false,
  lastSeq: -1,
  error: null,
  orphans: [],

  beginRun(runId, targets, preview) {
    const claimed = get()
      .orphans.filter((e) => e.runId === runId)
      .sort((a, b) => a.seq - b.seq);
    dropStaged();
    set({
      runId,
      runStatus: "running",
      preview,
      startedAt: Date.now(),
      durationMs: null,
      targets,
      outputs: [],
      summary: null,
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
    dropStaged();
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

    if (latestSeq >= 0 && event.seq !== latestSeq + 1) {
      console.warn(
        `[lyflow] 执行事件 seq 不连续：期望 ${latestSeq + 1}，收到 ${event.seq}。` +
          `界面继续更新，但可能漏了状态。`,
      );
    }
    latestSeq = event.seq;

    switch (event.kind) {
      case "run_started": {
        const nodes = new Map<string, NodeExecution>();
        const at = Date.now();
        for (const id of event.plan ?? []) {
          nodes.set(id, emptyNode());
          // 计划里的节点先全部亮成「排队中」。这一下也是一次状态变化，
          // 流水账里少了它，「节点依次变色」的序列就从 pending 开始了。
          for (const fn of transitionListeners) fn({ nodeId: id, state: "idle", at });
        }
        staged = nodes;
        useCacheStore.getState().setRanWith(event.nodes ?? []);
        flushNow();
        set({ targets: event.targets ?? [], outputs: event.outputs ?? [] });
        break;
      }
      case "plan_extended": {
        // 惰性闭包被 demand 了（ADR-0016）。这些节点不在 run_started 里，
        // 现在才进节点表；cacheKey 也要补进 ranWith，否则 stale 会漏判。
        const nodes = stage();
        const at = Date.now();
        for (const n of event.nodes) {
          if (nodes.has(n.id)) continue;
          nodes.set(n.id, emptyNode());
          for (const fn of transitionListeners) fn({ nodeId: n.id, state: "idle", at });
        }
        useCacheStore.getState().extendRanWith(event.nodes);
        scheduleFlush();
        break;
      }
      case "node_state": {
        const nodes = stage();
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
        const t: StateTransition = { nodeId: event.nodeId, state: event.state, at: Date.now() };
        for (const fn of transitionListeners) fn(t);
        scheduleFlush();
        break;
      }
      case "node_progress": {
        const nodes = stage();
        const prev = nodes.get(event.nodeId) ?? emptyNode();
        nodes.set(event.nodeId, { ...prev, progress: event.progress, message: event.message });
        scheduleFlush();
        break;
      }
      case "run_finished": {
        flushNow();
        set({
          runStatus: event.status as RunStatus,
          durationMs: event.durationMs ?? null,
          lastSeq: event.seq,
          // ADR-0022：成败判定的权威在这一份上，前端只显示不重建。
          summary: event.summary ?? null,
        });
        // 预览时不刷缓存统计：那是「事件到渲染」这条热路径上白多出来的一次 IPC
        if (!s.preview) void refreshCacheStats();
        break;
      }
      case "log": {
        const logs = s.logs.concat({
          seq: event.seq,
          level: event.level,
          nodeId: event.nodeId,
          message: event.message,
        });
        set({ logs: logs.length > LOG_LIMIT ? logs.slice(-LOG_LIMIT) : logs, lastSeq: event.seq });
        break;
      }
    }
  },

  markStale() {
    // 没跑过就无所谓过期。runStatus idle 时置 stale 会让节点无故变淡。
    if (get().runStatus === "idle" || get().stale) return;
    set({ stale: true });
  },

  reset() {
    dropStaged();
    set({
      runId: null,
      runStatus: "idle",
      preview: false,
      startedAt: null,
      durationMs: null,
      targets: [],
      outputs: [],
      summary: null,
      nodes: new Map(),
      logs: [],
      stale: false,
      lastSeq: -1,
      error: null,
      orphans: [],
    });
  },
}));

// ------------------------------------------------------- 按层聚合（F2）

const RANK: Record<NodeState, number> = {
  error: 6,
  running: 5,
  cancelled: 4,
  pending: 3,
  idle: 2,
  done: 1,
  skipped: 0,
};

/** 子图节点的状态 = 内部节点的归约：任一 error → error，任一 running → running，
 *  全 done/skipped → done（全 skipped 才算 skipped）。 */
function reduceExecutions(list: readonly NodeExecution[]): NodeExecution {
  let state: NodeState = "skipped";
  let duration = 0;
  let finished = 0;
  const errors: Diagnostic[] = [];
  for (const n of list) {
    if (RANK[n.state] > RANK[state]) state = n.state;
    duration += n.durationMs ?? 0;
    if (n.state === "done" || n.state === "skipped") finished += 1;
    errors.push(...n.errors);
  }
  // 全 done/skipped → done（全 skipped 才算 skipped）；有跑完的但还有没开始的 → pending
  if (state === "done" || state === "skipped") {
    state = list.every((n) => n.state === "skipped") ? "skipped" : "done";
  } else if (state === "idle" && finished > 0) {
    state = "pending";
  }
  return {
    state,
    durationMs: duration > 0 ? duration : undefined,
    progress: list.length > 0 ? finished / list.length : undefined,
    errors,
    children: { total: list.length, finished },
  };
}

function sameAggregate(a: NodeExecution | undefined, b: NodeExecution): boolean {
  if (!a) return false;
  return (
    a.state === b.state &&
    a.durationMs === b.durationMs &&
    a.progress === b.progress &&
    a.stats === b.stats &&
    a.message === b.message &&
    a.errors.length === b.errors.length &&
    a.children?.finished === b.children?.finished &&
    a.children?.total === b.children?.total
  );
}

let aggCache: {
  path: SubPath;
  nodes: ReadonlyMap<string, NodeExecution>;
  result: Map<string, NodeExecution>;
} | null = null;

/** 当前层级的「本地 id → 执行状态」。按对象身份缓存，一次事件批只算一遍。 */
export function aggregatedNodes(
  path: SubPath,
  nodes: ReadonlyMap<string, NodeExecution>,
): ReadonlyMap<string, NodeExecution> {
  if (aggCache && aggCache.path === path && aggCache.nodes === nodes) return aggCache.result;
  const prefix = pathPrefix(path);
  const groups = new Map<string, NodeExecution[]>();
  for (const [id, exec] of nodes) {
    if (prefix && !id.startsWith(prefix)) continue;
    const rest = id.slice(prefix.length);
    if (!rest) continue;
    const slash = rest.indexOf("/");
    const local = slash < 0 ? rest : rest.slice(0, slash);
    const list = groups.get(local);
    if (list) list.push(exec);
    else groups.set(local, [exec]);
  }
  const previous = aggCache?.result;
  const result = new Map<string, NodeExecution>();
  for (const [local, list] of groups) {
    // 叶子节点直接复用原对象，引用不变，节点组件就不会白重渲
    const merged = list.length === 1 ? list[0]! : reduceExecutions(list);
    const prev = previous?.get(local);
    result.set(local, prev && sameAggregate(prev, merged) ? prev : merged);
  }
  aggCache = { path, nodes, result };
  return result;
}

/** 某节点在**当前层级**的执行状态。节点组件和检查器都按本地 id 现查。 */
export function useNodeExecution(nodeId: string): NodeExecution | undefined {
  const path = useUiStore((s) => s.path);
  return useExecutionStore((s) => aggregatedNodes(path, s.nodes).get(nodeId));
}

const NO_ERRORS: ReadonlyMap<string, string> = new Map();

/** 某节点的 paramPath → 错误消息，ParamControls 据此画红框（交互清单 P0 #15）。 */
export function useParamErrors(nodeId: string): ReadonlyMap<string, string> {
  const node = useNodeExecution(nodeId);
  if (!node || node.errors.length === 0) return NO_ERRORS;
  const out = new Map<string, string>();
  for (const e of node.errors) {
    if (e.paramPath) out.set(e.paramPath, e.message);
  }
  return out.size === 0 ? NO_ERRORS : out;
}

/** 「done 7 / error 1」那一行。统计的是**展开后**的全部节点。 */
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

/** 本地的运行序号：两次 run_graph 走不同的 Tauri 工作线程，回复顺序不保证等于
 *  发起顺序。序号让后发的那次赢，与 C++ 侧「后开始的抢占先开始的」一致。 */
let runTicket = 0;

/** 宿主给的点云会话 id。`setRunSceneId` 由 `<LyFlowEditor sceneId>` 在渲染期装上，和
 *  `setTransport` / `setDialogs` 是同一类宿主配置：运行是从工具栏、快捷键、实时预览三处
 *  发起的，穿 props 会把它拖过整棵树。 */
let sceneId: string | null = null;

export function setRunSceneId(next: string | null): void {
  sceneId = next;
}

export function runSceneId(): string | null {
  return sceneId;
}

export interface RunRequest {
  targets?: string[] | undefined;
  /** 预览模式：源算子输出先抽稀，结果进独立缓存命名空间（ADR-0011）。 */
  preview?: boolean | undefined;
  previewMaxPoints?: number | undefined;
}

export async function startRun(
  doc: GraphDoc,
  graphPath: string | null,
  request: RunRequest = {},
): Promise<void> {
  const store = useExecutionStore.getState();
  const ticket = ++runTicket;
  const preview = request.preview === true;
  try {
    const runId = await transport.runGraph(doc, graphPath, {
      targets: request.targets,
      mode: preview ? "preview" : "full",
      previewMaxPoints: request.previewMaxPoints,
      sceneId,
    });
    if (ticket !== runTicket) return; // 已经有更晚的一次运行发起了，这次的回复作废
    store.beginRun(runId, request.targets ?? [], preview);
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
