// Live preview（ADR-0011）。拖参数时发 `mode=preview` 的普通 run，松手后按
// 「自动运行」开关补一次正式 run。抢占靠 M2 就定下的「新 run 取消旧 run」。

import { useCompareStore } from "../store/compare";
import { startRun, useExecutionStore } from "../store/execution";
import { useGraphStore } from "../store/graph";
import { useUiStore } from "../store/ui";
import { transport } from "../transport";
import { splitBind } from "./graphParams";
import { previewTargets } from "./nodeRun";
import type { SubPath } from "./subgraph";
import type { GraphDoc } from "../types/graph";

/** 拖的是顶层图参数（图参数那一行、配方矩阵里正在用的那一格）时交给预览的「节点 id」：跑它绑着的那几个顶层节点。
 *  以前这些控件没有节点 id，拖着不预览、松手也不补运行 —— 偏偏 gapOffset、纳入配方的阈值都在这里调。 */
const GRAPH_PARAM = "\u0001gp:";

export function graphParamPreviewId(name: string): string {
  return GRAPH_PARAM + name;
}

/** 这一下拖动要算到哪几个节点（完整 id）。拖的是节点参数：它和它下游正看着的（previewTargets）；拖的是图参数：
 *  它绑着的每一个顶层节点和它们下游正看着的，合在一起。 */
export function previewTargetsOf(
  doc: GraphDoc,
  path: SubPath,
  nodeId: string,
  watched: readonly ({ path: SubPath; nodeId: string } | null)[],
): string[] {
  if (!nodeId.startsWith(GRAPH_PARAM)) return previewTargets(doc, path, nodeId, watched);
  const binds = doc.params?.[nodeId.slice(GRAPH_PARAM.length)]?.binds ?? [];
  const bound = binds.map((b) => splitBind(b)?.node).filter((id): id is string => !!id);
  return [...new Set(bound.flatMap((id) => previewTargets(doc, [], id, watched)))];
}

/** 拖动过程中每一帧都在改值，攒一下再发。30 ms 是「跟手」和「别打死自己」的平衡点。 */
export const PREVIEW_DEBOUNCE_MS = 30;

let timer: ReturnType<typeof setTimeout> | null = null;
let lastNode: string | null = null;

function cancelPending(): void {
  if (timer === null) return;
  clearTimeout(timer);
  timer = null;
}

function fire(nodeId: string, preview: boolean): void {
  const graph = useGraphStore.getState();
  const ui = useUiStore.getState();
  if (graph.doc.nodes.length === 0) return;
  // 正看着的下游节点一起算：预览上的那个（与 Viewer3D 的 activeId 同一条规则：钉住的，没钉就是选中的那一个 ——
  // 参数面板里改别的节点时选中不变）、对比里没冻结的 B。不然拖上游的参数时画面不动
  const compare = useCompareStore.getState();
  const selected = ui.selectedNodes.size === 1 ? [...ui.selectedNodes][0]! : null;
  const active = ui.pinnedNode ?? selected;
  const watched = [active ? { path: ui.path, nodeId: active } : null, compare.on && !compare.snapshot ? compare.b : null];
  const targets = previewTargetsOf(graph.doc, ui.path, nodeId, watched);
  if (targets.length === 0) return; // 图参数没绑任何节点：没有可跑的
  void startRun(graph.doc, graph.filePath, {
    targets,
    preview,
    previewMaxPoints: preview ? ui.previewMaxPoints : undefined,
    auto: true,
  }).catch(() => {
    // 预览失败不打断编辑：正式运行时用户自然会看到同一条错误
  });
}

/** 参数开始拖动。进入预览态，节点上的状态条会标出来。 */
export function beginPreview(nodeId: string): void {
  if (transport.kind === "static") return;
  lastNode = nodeId;
  useUiStore.getState().setPreviewing(true);
}

/** 值变了：debounce 一次 preview run，目标是这个节点（和正看着的下游节点）。 */
export function schedulePreview(nodeId: string): void {
  if (transport.kind === "static") return;
  if (!useUiStore.getState().previewing) return;
  lastNode = nodeId;
  cancelPending();
  timer = setTimeout(() => {
    timer = null;
    fire(nodeId, true);
  }, PREVIEW_DEBOUNCE_MS);
}

/** 松手。开了自动运行就补一次正式 run —— 预览结果是抽稀过的，不能当结论。 */
export function endPreview(nodeId?: string): void {
  if (transport.kind === "static") return;
  const ui = useUiStore.getState();
  if (!ui.previewing) return;
  cancelPending();
  ui.setPreviewing(false);
  const node = nodeId ?? lastNode;
  if (!ui.autoRun || !node) return;
  // 预览那一次可能还在跑，正式 run 会把它抢占掉
  if (useExecutionStore.getState().runStatus === "running") {
    fire(node, false);
    return;
  }
  fire(node, false);
}
