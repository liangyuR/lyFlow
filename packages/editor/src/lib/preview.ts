// Live preview（ADR-0011）。拖参数时发 `mode=preview` 的普通 run，松手后按
// 「自动运行」开关补一次正式 run。抢占靠 M2 就定下的「新 run 取消旧 run」。
// 2026-10-04 起自动运行管所有改参数的方式：敲回车、下拉框、勾选框、↺、粘贴值、恢复、撤销……提交了一步就补
// （autoRunOnCommit，攒 AUTO_RUN_DEBOUNCE_MS 再发，只补跑过的节点）。

import { autoRunTargetsReady, paramEditTargets } from "./autoRun";
import { useCompareStore } from "../store/compare";
import { setBeforeExplicitRun, startRun, useExecutionStore } from "../store/execution";
import { useGraphStore, type CommittedChange } from "../store/graph";
import { useManifestStore } from "../store/manifest";
import { useRecipeStore } from "../store/recipe";
import { useUiStore } from "../store/ui";
import { transport } from "../transport";
import { effectiveGraphValues, splitBind } from "./graphParams";
import { closureOf, previewTargets } from "./nodeRun";
import { augmentOperators, fullId, locateEventNode, type SubPath } from "./subgraph";
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
  const out = new Set(bound.flatMap((id) => previewTargets(doc, [], id, watched)));
  // 看着的在子图里（进了子图拖上面的图参数）：previewTargets 只认同层与外层的，按包着它的顶层子图节点在不在下游算
  const down = closureOf(doc, bound, "down");
  for (const w of watched) if (w && w.path.length > 0 && down.includes(w.path[0]!.nodeId)) out.add(fullId(w.path, w.nodeId));
  return [...out];
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

/** 正看着的节点：预览上的那个（与 Viewer3D 的 activeId 同一条规则：钉住的，没钉就是选中的那一个 ——
 *  参数面板里改别的节点时选中不变）、对比里没冻结的 B。 */
function watchedNodes(): ({ path: SubPath; nodeId: string } | null)[] {
  const ui = useUiStore.getState();
  const compare = useCompareStore.getState();
  const selected = ui.selectedNodes.size === 1 ? [...ui.selectedNodes][0]! : null;
  const active = ui.pinnedNode ?? selected;
  return [active ? { path: ui.path, nodeId: active } : null, compare.on && !compare.snapshot ? compare.b : null];
}

// -- 提交了一步就补的自动运行：改过的节点（完整 id）与图参数先攒着，停手 AUTO_RUN_DEBOUNCE_MS 再一起发 ----------

/** 连按方向键微调、连按几次撤销只跑一次；短到敲完回车马上就有结果。 */
export const AUTO_RUN_DEBOUNCE_MS = 250;

let autoTimer: ReturnType<typeof setTimeout> | null = null;
const autoNodes = new Set<string>();
const autoParams = new Set<string>();

export function dropAutoRun(): void {
  if (autoTimer !== null) clearTimeout(autoTimer);
  autoTimer = null;
  autoNodes.clear();
  autoParams.clear();
}

/** 攒着的那些（再加上 extra：当前层级里的节点 id，或拖的图参数）要算到哪几个节点，正看着的下游一起。 */
function pendingTargets(doc: GraphDoc, extra: string | null): string[] {
  const ui = useUiStore.getState();
  const watched = watchedNodes();
  const out = new Set<string>(extra ? previewTargetsOf(doc, ui.path, extra, watched) : []);
  for (const id of autoNodes) {
    // 攒着的这段时间里节点被删了：不算它
    const at = locateEventNode(doc, id);
    if (at) for (const t of previewTargetsOf(doc, at.path, at.localId, watched)) out.add(t);
  }
  for (const name of autoParams) {
    if (doc.params?.[name]) for (const t of previewTargetsOf(doc, [], graphParamPreviewId(name), watched)) out.add(t);
  }
  return [...out];
}

/** 正式补一次：攒着的改动与 extra 合在一起发（拖动松手、拖框松手时 extra 是拖的那一个）。 */
function fireFormal(extra: string | null): void {
  const graph = useGraphStore.getState();
  if (graph.doc.nodes.length === 0) {
    dropAutoRun();
    return;
  }
  const targets = pendingTargets(graph.doc, extra);
  dropAutoRun();
  if (targets.length === 0) return; // 图参数没绑任何节点：没有可跑的
  void startRun(graph.doc, graph.filePath, { targets, auto: true }).catch(() => {
    // 自动补的失败不打断编辑：错误照样写在工具栏、节点上
  });
}

/** 提交了一步（graph store 的 onCommitted）：开着自动运行、改的是参数（有效值）、静音或图参数的取值，就攒起来补一次正式运行。
 *  拖动松手那一下 commit 时还在预览态：这里照样攒（共用的子图定义里改的，每个实例都在里面），紧接着的 endPreview 一起发、撤掉计时；
 *  只补跑过的节点 —— 从空白开始拼、新接的分支还没跑过时不替人开第一次（与拖框、切配方同一条）。 */
export function autoRunOnCommit(change: CommittedChange): void {
  if (transport.kind === "static") return;
  const ui = useUiStore.getState();
  if (!ui.autoRun) return;
  const exec = useExecutionStore.getState();
  if (exec.runStatus === "idle") return;
  const { before, after } = change;
  const ops = augmentOperators(useManifestStore.getState().operatorsById, { ...before.doc.subgraphs, ...after.doc.subgraphs });
  // 图参数的取值叠着当前配方：前后各按那时的配方集合算。按 id 认当前配方 —— 改名、撤销改名时前后的名字不一样
  const currentId = useRecipeStore.getState().currentId;
  const values = (doc: GraphDoc, recipes: CommittedChange["before"]["recipes"]) =>
    effectiveGraphValues(doc, (currentId === null ? undefined : recipes.recipes.find((r) => r.id === currentId))?.values ?? {});
  const edits = paramEditTargets(before.doc, after.doc, values(before.doc, before.recipes), values(after.doc, after.recipes), ops);
  // 正在跑的那一次里排着的节点开跑时先回到 idle（与 RoiLayer 拖框松手同一条）：跑着的时候不按状态筛
  const ready = exec.runStatus === "running" ? edits : autoRunTargetsReady(after.doc, edits, exec.nodes);
  if (ready.nodes.length === 0 && ready.graphParams.length === 0) return;
  for (const id of ready.nodes) autoNodes.add(id);
  for (const name of ready.graphParams) autoParams.add(name);
  if (autoTimer !== null) clearTimeout(autoTimer);
  autoTimer = setTimeout(() => {
    autoTimer = null;
    fireFormal(null);
  }, AUTO_RUN_DEBOUNCE_MS);
}

/** 2D 拖框 / 方向键微调之后这一组还有没设置的框（core 的校验必然拒掉，只会多一次红的运行）：撤掉这一步刚为它攒的。 */
export function skipAutoRunFor(fullNodeId: string): void {
  autoNodes.delete(fullNodeId);
  if (autoNodes.size === 0 && autoParams.size === 0) dropAutoRun();
}

// 人点的运行（与切配方的整图运行）读的就是现在的图：整图的已经包含攒着的那几处，撤掉；带 targets 的把它们并进来
// （不然「运行到这里」跑的是别处，改的那几处就没人跑了）
setBeforeExplicitRun((doc) => {
  const pending = autoTimer !== null || autoNodes.size > 0 || autoParams.size > 0 ? pendingTargets(doc, null) : [];
  dropAutoRun();
  return pending;
});
// 换了一张图：上一张攒着还没发的不能跑到这一张上（节点 id 常常同名）；关掉自动运行：攒着的也不发了
useGraphStore.subscribe((s, p) => {
  if (s.epoch !== p.epoch) dropAutoRun();
});
useUiStore.subscribe((s, p) => {
  if (p.autoRun && !s.autoRun) dropAutoRun();
});

function fire(nodeId: string, preview: boolean): void {
  const graph = useGraphStore.getState();
  const ui = useUiStore.getState();
  if (graph.doc.nodes.length === 0) return;
  // 正看着的下游节点一起算：预览上的那个、对比里没冻结的 B。不然拖上游的参数时画面不动
  const targets = previewTargetsOf(graph.doc, ui.path, nodeId, watchedNodes());
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

/** 拖完一下（2D 框）松手：开着自动运行就补一次正式运行，与拖滑块松手一样。拖框时不预览：底图是模板，每帧重跑没有可看的。 */
export function runAfterDrag(nodeId: string): void {
  if (transport.kind === "static" || !useUiStore.getState().autoRun) return;
  cancelPending();
  // 松手那一下的 commit 刚把这个框攒进了自动运行：合成这一次发，不跑两遍
  fireFormal(nodeId);
}

/** 图参数没绑任何节点：拖它没有可跑的，不进预览态（不然「预览中」亮着却什么都不跑）。 */
function nothingToPreview(nodeId: string): boolean {
  if (!nodeId.startsWith(GRAPH_PARAM)) return false;
  const gp = useGraphStore.getState().doc.params?.[nodeId.slice(GRAPH_PARAM.length)];
  return !gp || gp.binds.length === 0;
}

/** 参数开始拖动。进入预览态，节点上的状态条会标出来。 */
export function beginPreview(nodeId: string): void {
  if (transport.kind === "static" || nothingToPreview(nodeId)) return;
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
  // 预览那一次可能还在跑，正式 run 会把它抢占掉；之前攒着的自动运行（拖之前刚敲过回车）合进这一次
  fireFormal(node);
}
