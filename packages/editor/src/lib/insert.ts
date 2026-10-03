// 界面上「加节点」「插片段」的公共尾巴：调 store 的语义化动作、选中新节点、把自动连线的结果
// 用一句话告诉人（m8-plan L13 / L14）。拖到画布、面板双击、搜索面板三处都走这里。

import { useGraphStore, type AutoConnectResult } from "../store/graph";
import { findSnippet, useManifestStore } from "../store/manifest";
import { useUiStore } from "../store/ui";
import type { GraphDoc } from "../types/graph";
import type { SnippetDesc } from "../types/manifest";
import { augmentOperators, levelOf } from "./subgraph";
import { insertPortsFor, type GraphContext } from "./typecheck";

/** 节点没量到尺寸时按这么大算（与 lib/layout.ts 的兜底一致）。 */
const NODE_W = 220;
const NODE_H = 90;

/** 当前这一层（子图里就是那一层）与它的类型上下文。 */
function currentLevel(): { view: GraphDoc; ctx: GraphContext } {
  const doc = useGraphStore.getState().doc;
  const lvl = levelOf(doc, useUiStore.getState().path);
  const manifest = useManifestStore.getState();
  return {
    view: { ...doc, nodes: lvl.nodes, edges: lvl.edges },
    ctx: { operatorsById: augmentOperators(manifest.operatorsById, doc.subgraphs), typesByName: manifest.typesByName },
  };
}

/** 新加一个 opId 节点、插到 edgeId 那条线中间：加节点与插入一条撤销。at 不给就放在上下游两个节点中间。
 *  插不进（新算子没有唯一的一对端口两头都接得上）就整个不算，返回 null。 */
export function insertIntoEdge(opId: string, edgeId: string, at?: { x: number; y: number }): string | null {
  const graph = useGraphStore.getState();
  const before = currentLevel();
  const edge = before.view.edges.find((e) => e.id === edgeId);
  if (!edge) return null;
  let position = at;
  if (!position) {
    const up = before.view.nodes.find((n) => n.id === edge.from.node)?.ui?.position ?? { x: 0, y: 0 };
    const down = before.view.nodes.find((n) => n.id === edge.to.node)?.ui?.position ?? { x: up.x + 2 * NODE_W, y: up.y };
    position = { x: Math.round((up.x + NODE_W + down.x) / 2 - NODE_W / 2), y: Math.round((up.y + down.y) / 2 - NODE_H / 4) };
  }
  const where = position;
  const id = graph.batch("插入到连线中间", (cancel) => {
    const added = graph.addNode(opId, where);
    const { view, ctx } = currentLevel();
    const ports = added ? insertPortsFor(ctx, view, edge, added) : null;
    if (!added || !ports || !graph.insertOnEdge(edgeId, added, ports.inPort, ports.outPort)) {
      cancel();
      return null;
    }
    return added;
  });
  if (id) {
    useUiStore.getState().setSelection([id], []);
    useUiStore.getState().noteOperatorUsed(opId);
  }
  return id;
}

function report(result: AutoConnectResult, what: string): void {
  const ui = useUiStore.getState();
  if (result.nodeIds.length === 0) return;
  ui.setSelection(result.nodeIds, []);
  const parts: string[] = [];
  if (result.wired > 0) parts.push(`自动连了 ${result.wired} 条`);
  if (result.ambiguous.length > 0) {
    parts.push(`${result.ambiguous.length} 个输入有多个候选没连（已高亮）`);
  }
  if (result.missing.length > 0) parts.push(`缺算子 ${[...new Set(result.missing)].join(", ")}`);
  if (parts.length > 0) {
    ui.showToast(`${what}：${parts.join("，")}`, result.ambiguous.length > 0 || result.missing.length > 0 ? "warn" : "info");
  }
}

export function addNodeWithAutoConnect(
  opId: string,
  position: { x: number; y: number },
): AutoConnectResult {
  const result = useGraphStore.getState().addNodeAuto(opId, position);
  if (result.nodeIds.length > 0) useUiStore.getState().noteOperatorUsed(opId);
  report(result, "已添加");
  return result;
}

export function insertSnippet(snippet: SnippetDesc, at: { x: number; y: number }): AutoConnectResult {
  const result = useGraphStore.getState().insertSnippet(snippet, at);
  report(result, `已插入「${snippet.label}」`);
  return result;
}

export function insertSnippetById(id: string, at: { x: number; y: number }): AutoConnectResult | null {
  const snippet = findSnippet(id);
  if (!snippet) {
    useUiStore.getState().showToast(`片段不存在：${id}`, "warn");
    return null;
  }
  return insertSnippet(snippet, at);
}
