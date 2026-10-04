// 在出错的节点之间跳（工具栏的「error N」、F8 / Shift+F8）。错误挂在展开后的路径 id 上（ADR-0010）：
// 子图里的那个也要打开到它所在的一层（describeEventNode），参数上标红框。

import { describeEventNode, fullId, levelOf, type SubPath } from "./subgraph";
import { aggregatedNodes, useExecutionStore, type NodeExecution } from "../store/execution";
import { useGraphStore } from "../store/graph";
import { useManifestStore } from "../store/manifest";
import { useUiStore } from "../store/ui";
import type { NodeState } from "../types/execution";
import type { GraphDoc, GraphEdge } from "../types/graph";

/** 这次运行里出错的节点（error 状态、带诊断），按事件到达的顺序。上游失败连带的是 cancelled
 *  （core 的 upstream_failed），不在这里 —— 第一个就是根因。 */
export function errorNodeIds(): string[] {
  const out: string[] = [];
  for (const [id, n] of useExecutionStore.getState().nodes) {
    if (n.state === "error" && n.errors.length > 0) out.push(id);
  }
  return out;
}

/** step = 0 定位到第一个；1 / -1 从当前选中的那个往后 / 往前（绕回，没选中出错的节点时从头 / 从尾）。
 *  一个出错的都没有时返回 false。 */
export function revealError(step: 0 | 1 | -1): boolean {
  const ids = errorNodeIds();
  if (ids.length === 0) return false;
  let at = 0;
  if (step !== 0) {
    const ui = useUiStore.getState();
    const selected = ui.selectedNodes.size === 1 ? [...ui.selectedNodes][0]! : null;
    const current = selected === null ? -1 : ids.indexOf(fullId(ui.path, selected));
    at = current < 0 ? (step > 0 ? 0 : ids.length - 1) : (current + step + ids.length) % ids.length;
  }
  return revealEventNode(ids[at]!);
}

/** 定位到事件里的这个节点（展开后的路径 id）：打开到它所在的一层、选中，错误带参数路径就标到那一行。 */
function revealEventNode(id: string): boolean {
  const d = describeEventNode(useGraphStore.getState().doc, useManifestStore.getState().operatorsById, id);
  if (!d.reveal) return false;
  const { path, localId, exact } = d.reveal;
  const paramPath = exact ? useExecutionStore.getState().nodes.get(id)?.errors[0]?.paramPath : undefined;
  useUiStore.getState().revealNode(path, localId, paramPath);
  return true;
}

/** 定位到某一层上这个节点自己的错误。子图节点打开到里面出错的那个（errorSource）。 */
export function revealNodeError(path: SubPath, localId: string): boolean {
  const exec = aggregatedNodes(path, useExecutionStore.getState().nodes).get(localId);
  return revealEventNode(exec?.errorSource ?? fullId(path, localId));
}

/** 被上游连带取消的节点（cancelled + upstream_failed）是因为谁：沿入边往上找离它最近的出错节点，
 *  只穿过同样被连带取消的 —— 跑完了的节点（flow.fallback 这类 acceptsError 的）把上游的错接住了，根因不在那边。
 *  根因不在这一层（子图的输入在外面就断了）时返回 null。 */
export function failedUpstream(
  level: { edges: readonly GraphEdge[] },
  nodeId: string,
  stateOf: (id: string) => NodeState | undefined,
): string | null {
  const seen = new Set([nodeId]);
  let frontier = [nodeId];
  while (frontier.length > 0) {
    const next: string[] = [];
    for (const id of frontier) {
      for (const e of level.edges) {
        if (e.to.node !== id || seen.has(e.from.node)) continue;
        seen.add(e.from.node);
        const state = stateOf(e.from.node);
        if (state === "error") return e.from.node;
        if (state === "cancelled") next.push(e.from.node);
      }
    }
    frontier = next;
  }
  return null;
}

/** failedUpstream，这一层找不到就从子图节点那里往外一层接着找：子图的入口不是边，根因在子图外面时里面这一层看不到。
 *  返回根因所在的那一层和它在那一层的本地 id；哪一层都找不到返回 null。 */
export function culpritOf(
  doc: GraphDoc,
  path: SubPath,
  nodeId: string,
  nodes: ReadonlyMap<string, NodeExecution>,
): { path: SubPath; id: string } | null {
  let from = nodeId;
  for (let depth = path.length; depth >= 0; depth -= 1) {
    const at = depth === path.length ? path : path.slice(0, depth);
    const execs = aggregatedNodes(at, nodes);
    const hit = failedUpstream(levelOf(doc, at), from, (id) => execs.get(id)?.state);
    if (hit) return { path: at, id: hit };
    if (depth > 0) from = path[depth - 1]!.nodeId;
  }
  return null;
}
