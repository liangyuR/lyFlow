// 节点的几个基本编辑动作：快捷键（useShortcuts）与右键菜单（NodeContextMenu、空白处菜单）共用这一份，
// 免得两边各写一遍、行为慢慢走样（复制的范围、删除的撤销粒度、接通的提示）。

import { useGraphStore } from "../store/graph";
import { useManifestStore } from "../store/manifest";
import { currentOverrides } from "../store/recipe";
import { useUiStore } from "../store/ui";
import { copyText } from "./clipboard";
import { materializeBindings } from "./graphParams";
import { encodeNodeClipboard } from "./nodeClipboard";
import { planParamPaste, planParamReset, snapshotParams, type ParamEditPlan } from "./paramClipboard";
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
  let dropped: string[] = [];
  graph.batch(label, () => {
    if (edges.length > 0) graph.disconnect(edges);
    if (nodes.length > 0) dropped = graph.deleteNodes(nodes);
  });
  useUiStore.getState().clearSelection();
  if (dropped.length > 0) useUiStore.getState().showToast(droppedOutputsText(dropped), "warn");
}

/** 删掉的节点上标着图级输出：一并取消了，说一声（宿主按名字取它们）。 */
function droppedOutputsText(names: readonly string[]): string {
  return `图级输出 ${names.join("、")} 指着删掉的节点，一并取消了（Ctrl+Z 撤回）`;
}

/** 删掉这些节点、上下游按静音透传的规则接回去（一条撤销），提示接了几条、几条没接上。 */
export function deleteHealing(ids: readonly string[]): void {
  if (ids.length === 0) return;
  const ui = useUiStore.getState();
  const r = useGraphStore.getState().deleteNodesHealing(ids);
  ui.clearSelection();
  const left = r.unresolved > 0 ? `；${r.unresolved} 个下游没有合适的来源，没接` : "";
  const outs = r.dropped.length > 0 ? `；${droppedOutputsText(r.dropped)}` : "";
  ui.showToast(`已删除 ${ids.length} 个节点，接通 ${r.wired} 条${left}${outs}`, r.unresolved > 0 || outs ? "warn" : "info");
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

function opsNow() {
  return augmentOperators(useManifestStore.getState().operatorsById, useGraphStore.getState().doc.subgraphs);
}

/** 照着计划一条撤销写进去（setParam：被绑定的路由、稀疏写都在那里）。没有要改的就什么都不记。 */
function applyParamEdits(plan: ParamEditPlan, label: string): void {
  if (plan.edits.length === 0) return;
  const graph = useGraphStore.getState();
  graph.batch(label, () => {
    for (const e of plan.edits) graph.setParam(e.nodeId, e.name, e.value);
  });
}

/** 没动的那几样说清楚：不是同一种算子的节点、不归节点自己管的参数。 */
function leftAlone(plan: ParamEditPlan): string {
  const parts: string[] = [];
  if (plan.skippedOp > 0) parts.push(`${plan.skippedOp} 个节点不是同一种算子，没动`);
  if (plan.locked > 0) parts.push(`${plan.locked} 个参数由图参数 / 子图参数提供，没动`);
  return parts.length > 0 ? `；${parts.join("；")}` : "";
}

/** 右键「复制参数」：这个节点此刻的整组参数进参数剪贴板。没有参数的返回 false。 */
export function copyParams(nodeId: string): boolean {
  const ui = useUiStore.getState();
  const clip = snapshotParams(useGraphStore.getState().doc, ui.path, nodeId, opsNow(), currentOverrides());
  if (!clip) return false;
  ui.setParamClipboard(clip);
  ui.showToast(`已复制「${clip.from}」的 ${Object.keys(clip.values).length} 个参数`);
  return true;
}

/** 右键「粘贴参数」：写进这些节点里同一种算子的那几个，一条撤销。 */
export function pasteParams(ids: readonly string[]): void {
  const ui = useUiStore.getState();
  const clip = ui.paramClipboard;
  if (!clip) return;
  const plan = planParamPaste(useGraphStore.getState().doc, ui.path, clip, ids, opsNow());
  applyParamEdits(plan, plan.nodes === 1 ? "粘贴参数" : `粘贴参数到 ${plan.nodes} 个节点`);
  ui.showToast(
    plan.edits.length === 0
      ? `参数已经和「${clip.from}」一样${leftAlone(plan)}`
      : `已把「${clip.from}」的参数写进 ${plan.nodes} 个节点${leftAlone(plan)}`,
    plan.skippedOp > 0 || plan.locked > 0 ? "warn" : "info",
  );
}

/** 右键「全部恢复默认」：节点上写着的参数都删掉（回到算子默认值），一条撤销。 */
export function resetParams(ids: readonly string[]): void {
  const ui = useUiStore.getState();
  const plan = planParamReset(useGraphStore.getState().doc, ui.path, ids, opsNow());
  applyParamEdits(plan, plan.nodes === 1 ? "恢复默认参数" : `${plan.nodes} 个节点恢复默认参数`);
  ui.showToast(
    plan.edits.length === 0 ? `已经都是默认值${leftAlone(plan)}` : `${plan.nodes} 个节点的参数恢复了默认值${leftAlone(plan)}`,
    plan.locked > 0 ? "warn" : "info",
  );
}
