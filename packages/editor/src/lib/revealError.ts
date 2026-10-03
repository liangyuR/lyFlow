// 在出错的节点之间跳（工具栏的「error N」、F8 / Shift+F8）。错误挂在展开后的路径 id 上（ADR-0010）：
// 子图里的那个也要打开到它所在的一层（describeEventNode），参数上标红框。

import { describeEventNode, fullId, type SubPath } from "./subgraph";
import { aggregatedNodes, useExecutionStore } from "../store/execution";
import { useGraphStore } from "../store/graph";
import { useManifestStore } from "../store/manifest";
import { useUiStore } from "../store/ui";
import type { NodeState } from "../types/execution";
import type { GraphEdge } from "../types/graph";

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

/** 被上游连带取消的节点（cancelled + upstream_failed）是因为谁：沿入边往上找离它最近的出错节点。
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
        if (stateOf(e.from.node) === "error") return e.from.node;
        next.push(e.from.node);
      }
    }
    frontier = next;
  }
  return null;
}
