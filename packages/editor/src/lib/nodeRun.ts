// 「只运行此节点」（docs/node-run-plan.md）：标题栏按钮与右键菜单共用的那一套 ——
// 可用性预判（U4）、按钮的五态（U2 / U6）、发起与停止（U3）。两处各写一份的话，
// 判据迟早漂开，就会出现「按钮是灰的、菜单却能点」。

import { hasRelativePathParam } from "./params";
import { augmentOperators, fullId, levelOf, type SubPath } from "./subgraph";
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
import type { GraphDoc, GraphLevel } from "../types/graph";
import type { OperatorDesc } from "../types/manifest";

/** 按钮的 `data-run-state`（U6）。 */
export type NodeRunState = "idle" | "running" | "done" | "error" | "disabled";

/** 当前层里，这个节点每个**必需、非惰性**输入所连的上游（U4），按输入端口的声明顺序去重。
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

/** 上游有没有可用结果：本会话跑过（done / skipped）、输出可取、不 stale。纯提示，权威是 core 的 R2。
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

/** 这次运行是不是这个节点自己的「只运行此节点」发起的（U3：只有那时显示 ■、点了是停止）。 */
export function isOwnRun(isolate: readonly string[], full: string): boolean {
  return isolate.length === 1 && isolate[0] === full;
}

/** 五态的优先级：自己在跑（或被别的运行跑着）> 不可用 > 上一次的结果。
 *  running 放在 disabled 前面：全图运行跑到它时上游刚跑完，那时本来也可点（抢占，U3）。 */
export function nodeRunState(
  exec: NodeExecution | undefined,
  missing: readonly string[],
  ownRunning: boolean,
): NodeRunState {
  if (ownRunning || exec?.state === "running") return "running";
  if (missing.length > 0) return "disabled";
  if (exec && hasUsableResult(exec, false)) return "done";
  if (exec?.state === "error") return "error";
  return "idle";
}

/** 按钮的 title（U4 / U6）。names 是缺结果的上游给人看的名字。 */
export function nodeRunTitle(state: NodeRunState, names: readonly string[], own: boolean): string {
  if (own && state === "running") return "停止";
  if (state === "disabled") {
    return `上游 ${names.join("、")} 还没有可用结果 —— 先运行它们，或右键『运行到此』`;
  }
  return "只运行此节点（上游用已有结果）";
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

/** 不走 hook 的一次性判定：右键菜单打开那一刻用（U7「与按钮同一可用性」）。 */
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

/** 发起「只运行此节点」。id 是本层的，发出去的是展开后的路径（子图里点也说得清是哪一个）。
 *  与工具栏的运行共用那两道前置检查：空图、没保存却带相对路径。 */
export async function runNodeOnly(localId: string): Promise<void> {
  const graph = useGraphStore.getState();
  const ui = useUiStore.getState();
  if (!graph.filePath && hasRelativePathParam(graph.doc, useManifestStore.getState().operatorsById)) {
    ui.showToast("图里有相对路径参数，请先保存图（相对路径以图文件所在目录为基准）", "warn");
    return;
  }
  try {
    await startRun(graph.doc, graph.filePath, { isolate: [fullId(ui.path, localId)] });
  } catch (e) {
    ui.showToast(e instanceof Error ? e.message : String(e), "warn");
  }
}

/** 按钮被点了（U3）：自己发起的那次还在跑就停，否则发起一次（别的运行在跑就抢占它）。 */
export async function toggleNodeRun(localId: string): Promise<void> {
  const exec = useExecutionStore.getState();
  const full = fullId(useUiStore.getState().path, localId);
  if (exec.runStatus === "running" && isOwnRun(exec.isolate, full)) {
    try {
      await cancelCurrentRun();
    } catch (e) {
      useUiStore.getState().showToast(e instanceof Error ? e.message : String(e), "warn");
    }
    return;
  }
  await runNodeOnly(localId);
}
