// Live preview（ADR-0011）。拖参数时发 `mode=preview` 的普通 run，松手后按
// 「自动运行」开关补一次正式 run。抢占靠 M2 就定下的「新 run 取消旧 run」。

import { startRun, useExecutionStore } from "../store/execution";
import { useGraphStore } from "../store/graph";
import { useUiStore } from "../store/ui";
import { transport } from "../transport";
import { fullId } from "./subgraph";

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
  const target = fullId(ui.path, nodeId);
  void startRun(graph.doc, graph.filePath, {
    targets: [target],
    preview,
    previewMaxPoints: preview ? ui.previewMaxPoints : undefined,
  }).catch(() => {
    // 预览失败不打断编辑：正式运行时用户自然会看到同一条错误
  });
}

/** 参数开始拖动。进入预览态，节点上的状态条会标出来。 */
export function beginPreview(nodeId: string): void {
  if (transport.kind !== "tauri") return;
  lastNode = nodeId;
  useUiStore.getState().setPreviewing(true);
}

/** 值变了：debounce 一次 preview run，目标是这个节点。 */
export function schedulePreview(nodeId: string): void {
  if (transport.kind !== "tauri") return;
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
  if (transport.kind !== "tauri") return;
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
