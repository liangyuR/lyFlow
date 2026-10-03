// 执行状态 store。运行时状态不进 GraphDoc、不进撤销栈；runId 对不上的事件先攒进 orphans。
// M4：node_state / node_progress 按 16 ms 批量落库，事件 id 是路径、按层聚合（F2）。

import { create } from "zustand";

import { flashNodesLocate } from "../lib/motion";
import { localIdOf, locateEventNode, pathPrefix, type SubPath } from "../lib/subgraph";
import { transport } from "../transport";
import { refreshCacheStats, useCacheStore } from "./cache";
import { currentRecipeBlocker, runParamsOf } from "./recipe";
import { useUiStore } from "./ui";
import type {
  Diagnostic,
  ExecutionEvent,
  GraphDiagnostic,
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
  /** 聚合出来的（子图节点）：errors 里第一条来自哪个内部节点（事件里的完整路径 id）。
   *  节点上的错误文字据此写明是谁、点进去直接打开到它（顶层原来只看得到「这个子图红了」）。 */
  errorSource?: string | undefined;
  /** 聚合出来的（子图节点）：errors 每一条来自哪个内部节点，与 errors 一一对应（检查器里逐条写明、可点）。 */
  errorSources?: string[] | undefined;
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
  /** 节点表此刻反映的是哪一次运行的结果 —— 视图（主预览、连线查看器）按它取数，不按 runId。
   *  两者只在一种时候不一样：带 targets / isolate 的运行（预览、运行到此、单节点运行）发起之后、它的 run_started 到达之前。
   *  这时节点表留着上一次的结果，而新的 run 可能还在桥接层排队（ADR-0027：被抢占的那个还没退出）——
   *  core 里还没有它的任何结果，拿它去取只会取不到。排队的那个从没开跑就被取消时，它就一直是上一次的。 */
  resultRunId: string | null;
  runStatus: RunPhase;
  /** 本次运行是不是预览（ADR-0011）。正式结果到达后会覆盖它。 */
  preview: boolean;
  startedAt: number | null;
  durationMs: number | null;
  /** 本次运行是「只跑到某个节点」还是全图。空 = 全图。 */
  targets: string[];
  /** 单节点运行（docs/node-run-plan.md）的那几个，展开后的路径 id。空 = 普通运行。
   *  这时计划里其余的节点只是去结果仓取了一趟缓存：它们的状态与耗时照旧显示上一次的，
   *  下游不进计划、也照旧 —— 所以这种运行不清空节点表（验收 8）。
   *  修订一 V2：只要带 targets（运行到此、智能运行）都不清空节点表，只是计划里的节点照常更新。 */
  isolate: string[];
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
  /** 取消已经发出去、run_finished 还没到（停不下来的算子要等它自己跑完）。工具栏这时写「取消中…」。 */
  cancelling: boolean;
  /** 这一次运行是按什么发起的（展开后的路径 id）：「↻ 重跑」照它再来一次。 */
  request: RunRequest | null;

  beginRun(runId: string, targets: string[], preview: boolean, isolate?: string[], request?: RunRequest | null): void;
  failRun(message: string): void;
  apply(event: ExecutionEvent): void;
  markStale(): void;
  reset(): void;
}

const emptyNode = (): NodeExecution => ({ state: "idle", errors: [] });

/** 事件里的 nodeId 在不在 isolate 里：精确命中，或落在某个子图节点之下（R1 的前缀展开）。 */
export function inIsolate(isolate: readonly string[], nodeId: string): boolean {
  return isolate.some((t) => nodeId === t || nodeId.startsWith(`${t}/`));
}

/** 本次单节点运行里撞上 upstream_not_ready 的上游（展开后的 id）。开跑前的那批在
 *  run_finished.diagnostics 里；被 demand 的惰性上游是执行期才发现的，走 node_state，
 *  这里先攒着，run_finished 时一起提示（U5）。 */
let notReady: string[] = [];

/** 本次单节点运行里「在计划里、没重算、但输出照样可取」的节点（上游命中缓存的那些）。
 *  它们的 node_state 不落库，run_finished 时要靠它和 attached 一起判谁的输出还取得到（R7）。 */
let served = new Set<string>();

/** 本次部分运行（带 targets、非 isolate）里发过 node_state 的节点。收场时它们的显示都是这次的，
 *  不用再问 attached（V2）。 */
let touched = new Set<string>();

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
  resultRunId: null,
  runStatus: "idle",
  preview: false,
  startedAt: null,
  durationMs: null,
  targets: [],
  isolate: [],
  outputs: [],
  summary: null,
  nodes: new Map(),
  logs: [],
  stale: false,
  lastSeq: -1,
  error: null,
  orphans: [],
  cancelling: false,
  request: null,

  beginRun(runId, targets, preview, isolate = [], request = null) {
    const claimed = get()
      .orphans.filter((e) => e.runId === runId)
      .sort((a, b) => a.seq - b.seq);
    dropStaged();
    notReady = [];
    served = new Set();
    touched = new Set();
    // 带 targets 的运行都是「部分运行」：节点表留着（修订一 V2）
    const only = isolate.length > 0 || targets.length > 0;
    set({
      runId,
      // 全图运行清空节点表，结果当场就归新的这次；带 targets 的留着上一次的，等它的 run_started 再换
      resultRunId: only ? get().resultRunId : runId,
      runStatus: "running",
      preview,
      startedAt: Date.now(),
      durationMs: null,
      targets,
      isolate,
      outputs: [],
      summary: null,
      // 单节点运行不清节点表：其余节点的状态与耗时照旧（验收 8），「过时」标记也照旧 ——
      // 图的其余部分没有因为这次运行而变新
      nodes: only ? get().nodes : new Map(),
      logs: [],
      stale: only ? get().stale : false,
      lastSeq: -1,
      error: null,
      orphans: [],
      // 抢占了一个正在取消的：「取消中」到此为止
      cancelling: false,
      request,
    });
    // 认领在 run_graph 返回之前就到达的事件。
    for (const e of claimed) get().apply(e);
  },

  failRun(message) {
    dropStaged();
    set({ runId: null, runStatus: "error", error: message, durationMs: null, orphans: [], cancelling: false });
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
        // 真开跑了：从这一刻起节点表里的结果按这一次取（排队中的请求要等被抢占的那个退出，见 resultRunId）
        if (s.resultRunId !== event.runId) set({ resultRunId: event.runId });
        if (s.isolate.length === 0 && s.targets.length > 0) {
          // 部分运行（运行到此、智能运行，V2）：计划里的节点照常先亮成「排队中」，
          // 计划外的留着上一次的样子，收场时再按 attached 决定留不留
          const nodes = new Map(useExecutionStore.getState().nodes);
          const at = Date.now();
          for (const id of event.plan ?? []) {
            nodes.set(id, emptyNode());
            for (const fn of transitionListeners) fn({ nodeId: id, state: "idle", at });
          }
          staged = nodes;
          useCacheStore.getState().extendRanWith(event.nodes ?? []);
          flushNow();
          set({ outputs: event.outputs ?? [] });
          break;
        }
        if (s.isolate.length > 0) {
          // 单节点运行：节点表原样留着，只有 isolate 里的节点会在后面的 node_state 里变。
          // cacheKey 用追加而不是替换：计划外的下游还按它们上一次运行的键判 stale
          useCacheStore.getState().extendRanWith(event.nodes ?? []);
          set({ outputs: event.outputs ?? s.outputs });
          break;
        }
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
        if (s.isolate.length > 0 && !inIsolate(s.isolate, event.nodeId)) {
          // 单节点运行里其余节点只是去取了一趟缓存（R2）：它们的显示不跟着这次运行走，
          // 否则一次「只跑 b」会把 a 刷成「已缓存」、耗时清零。唯一要看的是执行期才发现的
          // upstream_not_ready（被 demand 的惰性上游），攒着留给 run_finished 提示
          if (event.state === "error" && event.errors?.[0]?.code === "upstream_not_ready") {
            notReady.push(event.nodeId);
          }
          if (
            (event.state === "done" || event.state === "skipped") &&
            event.stats?.outputsAvailable !== false
          ) {
            served.add(event.nodeId);
          }
          break;
        }
        const nodes = stage();
        const prev = nodes.get(event.nodeId) ?? emptyNode();
        touched.add(event.nodeId);
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
        if (s.targets.length > 0 && Array.isArray(event.attached)) {
          // 挂了过期旧结果的（修订二）也留着：输出取得到，stale 虚线框由 cache store 按键画
          dropUnreachable(s.isolate, [...event.attached, ...(event.attachedStale ?? [])]);
        }
        // 从没开跑就收场的（排在被抢占的运行后面，开跑前被取消或起不来，ADR-0027）：节点表还是被抢占的
        // 那一次的样子，而那一次后面的事件已经不归当前 runId、不会落库 —— 它当时在算的节点不能一直转圈
        if (s.resultRunId !== event.runId) settleAbandoned();
        set({
          runStatus: event.status as RunStatus,
          cancelling: false,
          durationMs: event.durationMs ?? null,
          lastSeq: event.seq,
          // ADR-0022：成败判定的权威在这一份上，前端只显示不重建。
          summary: event.summary ?? null,
        });
        // 预览时不刷缓存统计：那是「事件到渲染」这条热路径上白多出来的一次 IPC
        if (!s.preview) void refreshCacheStats();
        if (s.isolate.length > 0) reportNotReady(event.diagnostics);
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
    notReady = [];
    set({
      runId: null,
      resultRunId: null,
      runStatus: "idle",
      preview: false,
      startedAt: null,
      durationMs: null,
      targets: [],
      isolate: [],
      outputs: [],
      summary: null,
      nodes: new Map(),
      logs: [],
      stale: false,
      lastSeq: -1,
      error: null,
      orphans: [],
      cancelling: false,
      request: null,
    });
  },
}));

/** 部分运行收场（R7 / V2）：节点表里留着的那些，这次既没重算、没命中缓存、也没被 core 挂进来的
 *  （当前键的结果，或修订二的过期旧结果），
 *  按新 runId 已经取不到输出了 —— 退回 idle，不能显示「完成」却点开是空的。stale 不动（那是
 *  cache store 的事）。老 core 不带 attached，调用方不会走到这里。 */
function dropUnreachable(isolate: readonly string[], attached: readonly string[]): void {
  const keep = new Set(attached);
  const current = useExecutionStore.getState().nodes;
  let next: Map<string, NodeExecution> | null = null;
  const at = Date.now();
  for (const [id, exec] of current) {
    if (exec.state === "idle" || inIsolate(isolate, id) || served.has(id) || touched.has(id)) continue;
    if (keep.has(id)) continue;
    next ??= new Map(current);
    next.set(id, emptyNode());
    for (const fn of transitionListeners) fn({ nodeId: id, state: "idle", at });
  }
  served = new Set();
  touched = new Set();
  if (next) useExecutionStore.setState({ nodes: next });
}

/** 被抢占的那一次留在节点表里的半截状态：在算的、排着队的落成「已取消」—— 它确实被取消了
 *  （抢占就是取消），自己收场时发的也是这个，只是那些事件已经认不到当前的 runId 上。 */
function settleAbandoned(): void {
  const current = useExecutionStore.getState().nodes;
  let next: Map<string, NodeExecution> | null = null;
  const at = Date.now();
  for (const [id, exec] of current) {
    if (exec.state !== "running" && exec.state !== "pending") continue;
    next ??= new Map(current);
    next.set(id, { ...exec, state: "cancelled" });
    for (const fn of transitionListeners) fn({ nodeId: id, state: "cancelled", at });
  }
  if (next) useExecutionStore.setState({ nodes: next });
}

/** 单节点运行撞上「上游还没有可用结果」（U5）：不弹对话框，warn 级 toast，文案同 core 的那句；
 *  缺结果的上游在当前层各闪一下红光（不抖）。 */
function reportNotReady(diagnostics: readonly GraphDiagnostic[] | undefined): void {
  const ids = [
    ...(diagnostics ?? []).filter((d) => d.code === "upstream_not_ready").map((d) => d.nodeId),
    ...notReady,
  ].filter((id, i, all) => id !== "" && all.indexOf(id) === i);
  notReady = [];
  if (ids.length === 0) return;
  // 与 core 那句同一个模板（R2）。不直接用 run_finished.error：执行期才发现的那种，
  // error 是「N 个节点未能完成」
  const text =
    ids.length === 1
      ? `上游 ${ids[0]} 还没有可用结果，先运行它或运行到此`
      : `上游 ${ids.join("、")} 还没有可用结果，先运行它们或运行到此`;
  const ui = useUiStore.getState();
  ui.showToast(text, "warn");
  const path = ui.path;
  const local = ids.map((id) => localIdOf(path, id)).filter((id): id is string => id !== null);
  flashNodesLocate([...new Set(local)]);
}

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
function reduceExecutions(list: readonly NodeExecution[], ids: readonly string[]): NodeExecution {
  let state: NodeState = "skipped";
  let duration = 0;
  let finished = 0;
  const errors: Diagnostic[] = [];
  const errorSources: string[] = [];
  for (let i = 0; i < list.length; i += 1) {
    const n = list[i]!;
    if (RANK[n.state] > RANK[state]) state = n.state;
    duration += n.durationMs ?? 0;
    if (n.state === "done" || n.state === "skipped") finished += 1;
    errors.push(...n.errors);
    for (let k = 0; k < n.errors.length; k += 1) errorSources.push(n.errorSources?.[k] ?? ids[i]!);
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
    errorSource: errorSources[0],
    errorSources: errorSources.length > 0 ? errorSources : undefined,
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
    a.children?.total === b.children?.total &&
    a.errorSource === b.errorSource &&
    a.errorSources?.join("|") === b.errorSources?.join("|")
  );
}

/** 每个路径各缓存一份：对比模式下 A、B 可能在不同层级（compare-plan §1.7），单槽缓存会让两边
 *  轮流把对方挤掉 —— 子图节点每次都合并出新对象，订阅它的 selector 就每次拿到新引用。
 *  路径数组的引用是稳定的（ui.path、对比槽里存的那份），WeakMap 随它回收。 */
const aggCache = new WeakMap<SubPath, {
  nodes: ReadonlyMap<string, NodeExecution>;
  result: Map<string, NodeExecution>;
}>();

/** 当前层级的「本地 id → 执行状态」。按对象身份缓存，一次事件批只算一遍。 */
export function aggregatedNodes(
  path: SubPath,
  nodes: ReadonlyMap<string, NodeExecution>,
): ReadonlyMap<string, NodeExecution> {
  const cached = aggCache.get(path);
  if (cached && cached.nodes === nodes) return cached.result;
  const prefix = pathPrefix(path);
  const groups = new Map<string, { list: NodeExecution[]; ids: string[] }>();
  for (const [id, exec] of nodes) {
    if (prefix && !id.startsWith(prefix)) continue;
    const rest = id.slice(prefix.length);
    if (!rest) continue;
    const slash = rest.indexOf("/");
    const local = slash < 0 ? rest : rest.slice(0, slash);
    const group = groups.get(local);
    if (group) {
      group.list.push(exec);
      group.ids.push(id);
    } else {
      groups.set(local, { list: [exec], ids: [id] });
    }
  }
  const previous = cached?.result;
  const result = new Map<string, NodeExecution>();
  for (const [local, { list, ids }] of groups) {
    // 叶子节点直接复用原对象，引用不变，节点组件就不会白重渲。只有一个内部节点的子图也要归约 ——
    // 否则它的错误不知道是谁的
    const merged = list.length === 1 && ids[0] === prefix + local ? list[0]! : reduceExecutions(list, ids);
    const prev = previous?.get(local);
    result.set(local, prev && sameAggregate(prev, merged) ? prev : merged);
  }
  aggCache.set(path, { nodes, result });
  return result;
}

/** 某节点在**当前层级**的执行状态。节点组件和检查器都按本地 id 现查。 */
export function useNodeExecution(nodeId: string): NodeExecution | undefined {
  const path = useUiStore((s) => s.path);
  return useExecutionStore((s) => aggregatedNodes(path, s.nodes).get(nodeId));
}

/** 只取状态字符串。连线判断「数据在不在流」（docs/motion-plan.md E3）靠它：订阅整个
 *  NodeExecution 的话，每 50 ms 一条的进度都会让几百条边一起重渲。 */
export function useNodeState(nodeId: string): NodeState | undefined {
  const path = useUiStore((s) => s.path);
  return useExecutionStore((s) => aggregatedNodes(path, s.nodes).get(nodeId)?.state);
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

/** 本地的运行序号：让后发的那次赢，与桥接层「后来的请求抢占先来的」一致。Tauri 宿主里
 *  run_graph 在主线程上排队、按发起顺序回复，而且不再等被抢占的 run 退出（ADR-0027）；
 *  HTTP 宿主的回复顺序没有保证 —— 序号对两者都成立。 */
let runTicket = 0;

/** 宿主给的点云会话 id。`setRunSceneId` 由 `<LyFlowEditor sceneId>` 在渲染期装上，和
 *  `setTransport` / `setDialogs` 是同一类宿主配置：运行是从工具栏、快捷键、实时预览三处
 *  发起的，穿 props 会把它拖过整棵树。 */
let sceneId: string | null = null;

export function setRunSceneId(next: string | null): void {
  sceneId = next;
}

export interface RunRequest {
  targets?: string[] | undefined;
  /** 只运行这些节点（docs/node-run-plan.md R1），展开后的路径 id。给了它 targets 就不用再传。 */
  isolate?: string[] | undefined;
  /** 强制重算这些节点（修订一 V1），展开后的路径 id。 */
  force?: string[] | undefined;
  /** 预览模式：源算子输出先抽稀，结果进独立缓存命名空间（ADR-0011）。 */
  preview?: boolean | undefined;
  previewMaxPoints?: number | undefined;
  /** 顶层图参数的取值（param-recipe K3）。不给就用编辑器合成的「default + 当前配方覆盖」——
   *  界面上的每一次运行都是这样；给了就整份替换它（宿主或验收脚本要试一组别的值时用）。 */
  params?: Record<string, unknown> | undefined;
  /** 不是人点的（拖参数之后补的那一次、切配方时的自动运行）：工具栏不因它换成「↻ 重跑」，拖参数时按钮不闪。 */
  auto?: boolean | undefined;
}

export async function startRun(
  doc: GraphDoc,
  graphPath: string | null,
  request: RunRequest = {},
): Promise<void> {
  const store = useExecutionStore.getState();
  const ticket = ++runTicket;
  const preview = request.preview === true;
  const isolate = request.isolate ?? [];
  try {
    // 当前配方有 ①–③ 失配时不能运行（P3.7）：配方里多出来的名字 core 根本看不见（只交声明着的），
    // 类型不符、越界的 core 会报 bad_param，但原因在配方文件里，这里先说清楚是哪个配方的哪几处。
    // 显式给了 params 的（验收脚本整份替换取值）不受当前配方约束
    if (!request.params) {
      const blocker = currentRecipeBlocker(doc);
      if (blocker) throw new Error(blocker);
    }
    const runId = await transport.runGraph(doc, graphPath, {
      targets: request.targets,
      isolate: isolate.length > 0 ? isolate : undefined,
      force: request.force && request.force.length > 0 ? request.force : undefined,
      mode: preview ? "preview" : "full",
      previewMaxPoints: request.previewMaxPoints,
      params: request.params ?? runParamsOf(doc),
      sceneId,
    });
    if (ticket !== runTicket) return; // 已经有更晚的一次运行发起了，这次的回复作废
    // 给了 isolate 时 core 的 targets 就是同一组（R1），这边也照这个记
    store.beginRun(runId, isolate.length > 0 ? isolate : (request.targets ?? []), preview, isolate, request);
  } catch (e) {
    if (ticket !== runTicket) throw e;
    store.failRun(e instanceof Error ? e.message : String(e));
    throw e;
  }
}

export async function cancelCurrentRun(): Promise<void> {
  const { runId, runStatus, cancelling } = useExecutionStore.getState();
  // 已经在取消了：再按一次 Esc 不再发一遍
  if (!runId || runStatus !== "running" || cancelling) return;
  useExecutionStore.setState({ cancelling: true });
  try {
    await transport.cancelRun(runId);
  } catch (e) {
    if (useExecutionStore.getState().runId === runId) useExecutionStore.setState({ cancelling: false });
    throw e;
  }
}

/** 「↻ 重跑」：停掉这一次、按同样的范围重新开始（运行到此、单节点、强制重算的目标都照旧；被抢占的那次由桥接层取消，
 *  ADR-0027）。目标节点在跑的时候被删掉了就退回运行整张图并说一声。没有在跑的就是一次普通的全图运行。 */
export async function restartRun(doc: GraphDoc, graphPath: string | null): Promise<void> {
  const { runStatus, request } = useExecutionStore.getState();
  if (runStatus !== "running" || !request) return startRun(doc, graphPath, {});
  const alive = (ids: string[] | undefined) => ids?.filter((id) => locateEventNode(doc, id) !== null);
  const targets = alive(request.targets);
  const isolate = alive(request.isolate);
  const aimed = (request.targets?.length ?? 0) + (request.isolate?.length ?? 0) > 0;
  const lost = aimed && (targets?.length ?? 0) + (isolate?.length ?? 0) === 0;
  if (lost) {
    useUiStore.getState().showToast("原来要跑到的节点已经删掉了，改成运行整张图", "warn");
    return startRun(doc, graphPath, {});
  }
  return startRun(doc, graphPath, { ...request, targets, isolate, force: alive(request.force), auto: undefined });
}

/** 工具栏上运行 / 取消两个按钮此刻的样子。人发起的运行在跑时「运行」变「↻ 重跑」；预览与自动补的那一次不变（拖参数时不闪）。 */
export function runControlsOf(s: {
  runStatus: RunPhase;
  preview: boolean;
  cancelling: boolean;
  request: RunRequest | null;
}): { run: "run" | "rerun"; cancel: "off" | "on" | "cancelling" } {
  const running = s.runStatus === "running";
  return {
    run: running && !s.preview && !s.request?.auto ? "rerun" : "run",
    cancel: !running ? "off" : s.cancelling ? "cancelling" : "on",
  };
}
