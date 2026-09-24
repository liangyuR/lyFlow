// 节点标题栏的运行按钮与右键菜单共用的那一套（docs/node-run-plan.md，§6 修订一）：
// 预判（会一并跑哪些上游、是不是已是最新）、按钮的五态、三种发起方式与停止。两处各写一份的话，
// 判据迟早漂开，就会出现「按钮这么说、菜单那么做」。
//
// 修订一之后主操作是**智能运行**：单击 = `targets: [id]`（运行到此的语义），系统自己算出最少要跑
// 哪些节点；Shift+单击再加 `force: [id]`；「仅此节点」（isolate）降为右键里的次级操作。

import { hasRelativePathParam } from "./params";
import { augmentOperators, fullId, levelOf, localIdOf, type SubPath } from "./subgraph";
import { staleLocalIds, useCacheStore } from "../store/cache";
import {
  aggregatedNodes,
  cancelCurrentRun,
  startRun,
  useExecutionStore,
  type NodeExecution,
} from "../store/execution";
import { useGraphStore } from "../store/graph";
import { useManifestStore } from "../store/manifest";
import { useUiStore } from "../store/ui";
import type { PlanNode } from "../types/execution";
import type { GraphDoc, GraphLevel } from "../types/graph";
import type { OperatorDesc } from "../types/manifest";

/** 按钮的 `data-run-state`（U6，V5 起 disabled 只剩「本节点有校验错误」一种）。 */
export type NodeRunState = "idle" | "running" | "done" | "error" | "disabled";

// ------------------------------------------------------------------ 图结构

/** 当前层里，这个节点每个**必需、非惰性**输入所连的上游（U4），按输入端口的声明顺序去重。
 *  只给「仅此节点」（isolate）的可用性用：它要求直接上游都已经有结果。
 *  子图里从子图输入口进来的那一路在这一层没有边，不算 —— 那由 core 的 R2 把关。 */
export function runUpstreamOf(level: GraphLevel, nodeId: string, op: OperatorDesc): string[] {
  const out: string[] = [];
  for (const port of op.inputs) {
    if (port.required === false || port.lazy === true) continue;
    for (const e of level.edges) {
      if (e.to.node !== nodeId || e.to.port !== port.name) continue;
      if (!out.includes(e.from.node)) out.push(e.from.node);
    }
  }
  return out;
}

/** 当前层里这个节点的全部祖先（沿所有入边往上），离源头近的排前面 —— 提示里「将一并运行上游
 *  A、B」照数据流向念。智能运行的计划就是这个闭包（core 的 targets 语义）。 */
export function ancestorsOf(level: GraphLevel, nodeId: string): string[] {
  const depth = new Map<string, number>();
  let frontier = [nodeId];
  for (let d = 1; frontier.length > 0 && d <= level.nodes.length; d += 1) {
    const next: string[] = [];
    for (const id of frontier) {
      for (const e of level.edges) {
        if (e.to.node !== id || e.from.node === nodeId) continue;
        const seen = depth.get(e.from.node);
        if (seen !== undefined && seen >= d) continue;
        depth.set(e.from.node, d);
        next.push(e.from.node);
      }
    }
    frontier = next;
  }
  return [...depth.entries()].sort((a, b) => b[1] - a[1]).map(([id]) => id);
}

// ------------------------------------------------------------------ 预判

/** 上游有没有可用结果：本会话跑过（done / skipped）、输出可取、不 stale。
 *  子图节点的聚合状态不带 stats，done / skipped 已经说明内部全跑完了。 */
export function hasUsableResult(exec: NodeExecution | undefined, stale: boolean): boolean {
  if (!exec || stale) return false;
  if (exec.state !== "done" && exec.state !== "skipped") return false;
  return exec.stats?.outputsAvailable !== false;
}

export function missingUpstream(
  upstream: readonly string[],
  execs: ReadonlyMap<string, NodeExecution>,
  stale: ReadonlySet<string>,
): string[] {
  return upstream.filter((u) => !hasUsableResult(execs.get(u), stale.has(u)));
}

/** 预判要用到的全部现状。plan 是 `plan_graph` 的结论（ADR-0007，只有 Tauri 那条路有），
 *  拿不到时退回执行状态。都是提示 —— 权威是 core 真编出来的计划。 */
export interface RunForecastInput {
  path: SubPath;
  plan: ReadonlyMap<string, PlanNode>;
  planUsable: boolean;
  execs: ReadonlyMap<string, NodeExecution>;
  stale: ReadonlySet<string>;
}

/** 本层节点对应的计划条目（子图节点展开成它的全部内部节点）。惰性与静音的不算：前者这次
 *  多半不跑，后者只是透传、不花力气。 */
function planEntries(input: RunForecastInput, local: string): PlanNode[] {
  const out: PlanNode[] = [];
  for (const n of input.plan.values()) {
    if (n.lazy === true || n.bypass) continue;
    if (localIdOf(input.path, n.nodeId) === local) out.push(n);
  }
  return out;
}

/** 这个节点下次运行会不会真算（而不是命中缓存）。 */
export function willCompute(input: RunForecastInput, local: string): boolean {
  if (input.planUsable) {
    const entries = planEntries(input, local);
    if (entries.length > 0) return entries.some((n) => !n.cached);
  }
  return !hasUsableResult(input.execs.get(local), input.stale.has(local));
}

/** 单击会一并跑哪些上游（V5 的 `data-run-upstream`）。 */
export function upstreamToRun(input: RunForecastInput, ancestors: readonly string[]): string[] {
  return ancestors.filter((a) => willCompute(input, a));
}

// ------------------------------------------------------------------ 状态与文案

/** 这次运行是不是这个节点自己的按钮（或同义的「运行到此」）发起的（V7：只有那时显示 ■、点了是停止）。 */
export function isOwnRun(targets: readonly string[], full: string): boolean {
  return targets.length === 1 && targets[0] === full;
}

/** 五态的优先级：自己在跑（或被别的运行跑着）> 本节点有校验错误 > 上一次的结果。 */
export function nodeRunState(
  exec: NodeExecution | undefined,
  invalid: boolean,
  ownRunning: boolean,
): NodeRunState {
  if (ownRunning || exec?.state === "running") return "running";
  if (invalid) return "disabled";
  if (exec && hasUsableResult(exec, false)) return "done";
  if (exec?.state === "error") return "error";
  return "idle";
}

export const TITLE_RUN = "运行此节点";
export const TITLE_UP_TO_DATE = "已是最新（命中缓存）—— Shift+点击强制重算";
export const TITLE_STOP = "停止";
export const TITLE_INVALID = "此节点有校验错误，先修正参数再运行";

/** 按钮的 title（V5）。names 是将一并运行的上游给人看的名字。 */
export function nodeRunTitle(opts: {
  state: NodeRunState;
  own: boolean;
  upstream: readonly string[];
  upToDate: boolean;
}): string {
  if (opts.own && opts.state === "running") return TITLE_STOP;
  if (opts.state === "disabled") return TITLE_INVALID;
  if (opts.upstream.length > 0) return `${TITLE_RUN}（将一并运行上游 ${opts.upstream.join("、")}）`;
  if (opts.upToDate) return TITLE_UP_TO_DATE;
  return TITLE_RUN;
}

/** 「仅此节点」不可用时的说明（沿用 U4 的文案）。 */
export function isolateUnavailableTitle(names: readonly string[]): string {
  return `上游 ${names.join("、")} 还没有可用结果 —— 先运行它们，或用『运行到此』`;
}

/** 节点给人看的名字：改过名就用改的，否则是算子的 label，都没有就是 id。 */
export function nodeLabel(
  level: GraphLevel,
  id: string,
  operatorsById: ReadonlyMap<string, OperatorDesc>,
): string {
  const node = level.nodes.find((n) => n.id === id);
  if (!node) return id;
  return node.ui?.title ?? operatorsById.get(node.op)?.label ?? id;
}

/** 不走 hook 的一次性判定：右键「仅此节点」打开那一刻用（直接上游都得有结果）。 */
export function nodeRunAvailability(
  localId: string,
): { missing: string[]; names: string[]; available: boolean } {
  const doc: GraphDoc = useGraphStore.getState().doc;
  const path: SubPath = useUiStore.getState().path;
  const operatorsById = augmentOperators(useManifestStore.getState().operatorsById, doc.subgraphs);
  const level = levelOf(doc, path);
  const node = level.nodes.find((n) => n.id === localId);
  const op = node ? operatorsById.get(node.op) : undefined;
  if (!node || !op) return { missing: [], names: [], available: false };
  const execs = aggregatedNodes(path, useExecutionStore.getState().nodes);
  const { plan, ranWith } = useCacheStore.getState();
  const missing = missingUpstream(runUpstreamOf(level, localId, op), execs, staleLocalIds(path, plan, ranWith));
  return {
    missing,
    names: missing.map((id) => nodeLabel(level, id, operatorsById)),
    available: missing.length === 0,
  };
}

// ------------------------------------------------------------------ 发起

/** 与工具栏的运行共用那道前置检查：没保存却带相对路径就跑不了。 */
function blockedByRelativePaths(): boolean {
  const graph = useGraphStore.getState();
  if (graph.filePath || !hasRelativePathParam(graph.doc, useManifestStore.getState().operatorsById)) {
    return false;
  }
  useUiStore
    .getState()
    .showToast("图里有相对路径参数，请先保存图（相对路径以图文件所在目录为基准）", "warn");
  return true;
}

async function launch(request: { targets?: string[]; isolate?: string[]; force?: string[] }): Promise<void> {
  if (blockedByRelativePaths()) return;
  const graph = useGraphStore.getState();
  try {
    await startRun(graph.doc, graph.filePath, request);
  } catch (e) {
    useUiStore.getState().showToast(e instanceof Error ? e.message : String(e), "warn");
  }
}

/** 智能运行（V3）：本节点 + 缺结果或过时的上游，有缓存的复用，下游不进计划。
 *  force = Shift+单击（V4）：本节点跳过缓存，上游照旧智能判断。id 是本层的，发出去的是展开后的路径。 */
export function runNodeSmart(localId: string, force = false): Promise<void> {
  const full = fullId(useUiStore.getState().path, localId);
  return launch(force ? { targets: [full], force: [full] } : { targets: [full] });
}

/** 仅此节点（V6）：isolate —— 上游只许用已有结果，不齐由 core 回 upstream_not_ready。 */
export function runNodeOnly(localId: string): Promise<void> {
  return launch({ isolate: [fullId(useUiStore.getState().path, localId)] });
}

/** 按钮被点了（V7）：自己发起的那次还在跑就停，否则发起一次（别的运行在跑就抢占它）。 */
export async function toggleNodeRun(localId: string, force = false): Promise<void> {
  const exec = useExecutionStore.getState();
  const full = fullId(useUiStore.getState().path, localId);
  if (exec.runStatus === "running" && isOwnRun(exec.targets, full)) {
    try {
      await cancelCurrentRun();
    } catch (e) {
      useUiStore.getState().showToast(e instanceof Error ? e.message : String(e), "warn");
    }
    return;
  }
  await runNodeSmart(localId, force);
}
