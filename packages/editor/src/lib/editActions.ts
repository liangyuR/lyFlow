// 节点的几个基本编辑动作：快捷键（useShortcuts）与右键菜单（NodeContextMenu、空白处菜单）共用这一份，
// 免得两边各写一遍、行为慢慢走样（复制的范围、删除的撤销粒度、接通的提示）。

import { useGraphStore } from "../store/graph";
import { useManifestStore } from "../store/manifest";
import { currentOverrides } from "../store/recipe";
import { useUiStore } from "../store/ui";
import { copyText } from "./clipboard";
import { materializeBindings } from "./graphParams";
import { encodeNodeClipboard } from "./nodeClipboard";
import { augmentOperators, levelOf } from "./subgraph";

/** 复制（cut 时再删掉）这一层里的这些节点。只带走两端都在里面的边 —— 粘贴时内部连线得以保留。
 *  应用内一份，系统剪贴板也写一份（另一个窗口、重开之后照样粘得进来）。返回复制了几个。 */
export function copyNodes(ids: ReadonlySet<string>, cut: boolean): number {
  const graph = useGraphStore.getState();
  const ui = useUiStore.getState();
  const doc = graph.doc;
  // 从当前这一层取：选区是本层的 id。以前取的是顶层的 doc.nodes —— 在子图里复制要么一个都没有，
  // 要么拿到顶层同名的那个节点；剪切更糟，剪贴板里是错的，本层的节点却真删了
  const level = levelOf(doc, ui.path);
  const ops = augmentOperators(useManifestStore.getState().operatorsById, doc.subgraphs);
  const nodes = materializeBindings(doc, ui.path, level.nodes.filter((n) => ids.has(n.id)), ops, currentOverrides());
  const edges = level.edges.filter((edge) => ids.has(edge.from.node) && ids.has(edge.to.node));
  const clip = { nodes: JSON.parse(JSON.stringify(nodes)), edges: JSON.parse(JSON.stringify(edges)) };
  ui.setClipboard(clip);
  void copyText(encodeNodeClipboard(clip)).then((ok) => useUiStore.setState({ clipboardOnlyInApp: !ok }));
  if (cut) graph.deleteNodes([...ids]);
  ui.showToast(`已${cut ? "剪切" : "复制"} ${nodes.length} 个节点`);
  return nodes.length;
}

/** 删掉这些节点与连线，一条撤销（框选会把相连的边一起选上：以前先断边、再删节点记成两条）。清掉选中。 */
export function deleteSelection(nodes: readonly string[], edges: readonly string[]): void {
  if (nodes.length === 0 && edges.length === 0) return;
  const graph = useGraphStore.getState();
  const label =
    nodes.length > 0
      ? nodes.length === 1 ? "删除节点" : `删除 ${nodes.length} 个节点`
      : edges.length === 1 ? "断开连线" : `断开 ${edges.length} 条连线`;
  graph.batch(label, () => {
    if (edges.length > 0) graph.disconnect(edges);
    if (nodes.length > 0) graph.deleteNodes(nodes);
  });
  useUiStore.getState().clearSelection();
}

/** 删掉这些节点、上下游按静音透传的规则接回去（一条撤销），提示接了几条、几条没接上。 */
export function deleteHealing(ids: readonly string[]): void {
  if (ids.length === 0) return;
  const ui = useUiStore.getState();
  const r = useGraphStore.getState().deleteNodesHealing(ids);
  ui.clearSelection();
  const left = r.unresolved > 0 ? `；${r.unresolved} 个下游没有合适的来源，没接` : "";
  ui.showToast(`已删除 ${ids.length} 个节点，接通 ${r.wired} 条${left}`, r.unresolved > 0 ? "warn" : "info");
}

/** 断开这些节点的全部连线（一条撤销）。返回断了几条。 */
export function disconnectAll(ids: readonly string[]): number {
  const ui = useUiStore.getState();
  const kill = new Set(ids);
  const edges = levelOf(useGraphStore.getState().doc, ui.path).edges.filter((e) => kill.has(e.from.node) || kill.has(e.to.node));
  if (edges.length === 0) return 0;
  useGraphStore.getState().disconnect(edges.map((e) => e.id));
  return edges.length;
}

/** 选中这一层里与 nodeId 同一种算子的全部节点（多选表单只在同一种算子时出现）。返回选上几个。 */
export function selectSameOp(nodeId: string): number {
  const ui = useUiStore.getState();
  const level = levelOf(useGraphStore.getState().doc, ui.path);
  const op = level.nodes.find((n) => n.id === nodeId)?.op;
  if (!op) return 0;
  const ids = level.nodes.filter((n) => n.op === op).map((n) => n.id);
  ui.setSelection(ids, []);
  return ids.length;
}
