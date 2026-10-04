// 调参记录的 store（纯函数在 lib/runHistory）。startRun 拿到 runId 时记下交给 core 的那份图与图参数；
// run_started 补上这次算了哪几个节点；收场（不是预览）时补上状态、耗时与量测读数。只在本次会话里，换一张图（打开、新建）就清空。

import { create } from "zustand";

import { runReadingsOf, type RunRecord } from "../lib/runHistory";
import type { NodeExecution, RunRequest } from "./execution";
import type { RunStatus } from "../types/execution";
import type { GraphDoc } from "../types/graph";

/** 留最近这么多次。更早的调参过程回头看的少。 */
export const MAX_RUN_RECORDS = 50;

interface RunHistoryState {
  records: readonly RunRecord[];
  /** 这张图打开以来第几次运行（只数记下来的那些）。 */
  seq: number;
  /** 基准（runId）：调到一组好的之后接着试，每一次都与它比，而不只是与上一次比。不随 50 条的上限被挤掉。 */
  baseline: string | null;
  setBaseline(runId: string | null): void;
  begin(
    runId: string,
    doc: GraphDoc,
    params: Readonly<Record<string, unknown>> | undefined,
    recipe: string | null,
    request: RunRequest,
    recipeId?: string | null,
  ): void;
  /** run_started 的计划（完整 id）：运行到某个节点时只有这些是这一次算的，读数只记它们。 */
  planned(runId: string, plan: readonly string[]): void;
  finish(runId: string, status: RunStatus, durationMs: number | null, nodes: ReadonlyMap<string, NodeExecution>): void;
  clear(): void;
}

/** 每条记录的计划（不进 RunRecord：只在收场时用一次）。 */
const plans = new Map<string, ReadonlySet<string>>();

export const useRunHistoryStore = create<RunHistoryState>((set, get) => ({
  records: [],
  seq: 0,
  baseline: null,
  setBaseline(runId) {
    if (get().baseline === runId) return;
    if (runId !== null && !get().records.some((r) => r.runId === runId)) return;
    set({ baseline: runId });
  },
  begin(runId, doc, params, recipe, request, recipeId = null) {
    // 被这一次顶掉的（抢占、排队时被取消）收不到自己的收场：还挂着「运行中」的记成取消 —— 预览、单节点运行也会顶掉它
    const prev = get().records;
    const settled = prev.some((r) => r.status === "running")
      ? prev.map((r) => (r.status === "running" ? { ...r, status: "cancelled" as const } : r))
      : prev;
    // 预览是抽稀过的，读数不能当结论；单节点运行（isolate）只算一个节点，与前后两次比不出东西
    if (request.preview || (request.isolate?.length ?? 0) > 0) {
      if (settled !== prev) set({ records: settled });
      return;
    }
    const seq = get().seq + 1;
    const record: RunRecord = {
      seq,
      runId,
      at: Date.now(),
      doc,
      params,
      recipe,
      recipeId,
      targets: request.targets ?? [],
      status: "running",
      durationMs: null,
      readings: [],
    };
    const all = [record, ...settled];
    // 留最近的 50 条；基准被挤到外面的话占掉第 50 个位置
    const baseline = get().baseline;
    const keep = all.slice(0, MAX_RUN_RECORDS);
    const base = baseline && !keep.some((r) => r.runId === baseline) ? all.find((r) => r.runId === baseline) : undefined;
    const records = base ? [...keep.slice(0, MAX_RUN_RECORDS - 1), base] : keep;
    for (const dropped of all) if (!records.includes(dropped)) plans.delete(dropped.runId);
    set({ seq, records });
  },
  planned(runId, plan) {
    if (get().records.some((r) => r.runId === runId)) plans.set(runId, new Set(plan));
  },
  finish(runId, status, durationMs, nodes) {
    const records = get().records;
    const i = records.findIndex((r) => r.runId === runId);
    if (i < 0) return;
    const record = records[i]!;
    // 运行到某个节点时节点表里留着上一次的别的节点：只记这一次算了的。没收到 run_started（编译就失败了、排队时被取消）
    // 的就一个都没算，节点表里的全是上一次的
    const plan = record.targets.length > 0 ? (plans.get(runId) ?? new Set<string>()) : undefined;
    const readings = runReadingsOf(nodes).filter((r) => !plan || plan.has(r.id));
    plans.delete(runId);
    const next = records.slice();
    next[i] = { ...record, status, durationMs, readings };
    set({ records: next });
  },
  clear() {
    plans.clear();
    if (get().records.length === 0 && get().seq === 0 && get().baseline === null) return;
    set({ records: [], seq: 0, baseline: null });
  },
}));
