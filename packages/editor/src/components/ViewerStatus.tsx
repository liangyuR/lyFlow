// 预览区的空态。以前只有一句「未运行」「该节点运行出错」「该节点尚未产出结果」，下一步得自己想、自己去找：
// 现在跟着给动作 —— 没跑过 / 这次没跑到 → 「运行到此节点」；出错 → 错误原文 + 定位到出错的参数；
// 被上游连带没执行 → 说是哪个节点出的错 + 定位过去。文字仍在 viewer3d-status 里（验收脚本读它），动作另起一行。

import { keyHint } from "../lib/keymap";
import { nodeLabel } from "../lib/nodeRun";
import { culpritOf, revealError, revealNodeError } from "../lib/revealError";
import { augmentOperators, levelOf } from "../lib/subgraph";
import { useNodeStale } from "../store/cache";
import { aggregatedNodes, useExecutionStore } from "../store/execution";
import { useGraphStore } from "../store/graph";
import { useManifestStore } from "../store/manifest";
import { useUiStore } from "../store/ui";

/** 参数改过、还没重跑：画面上还是上一次的结果。以前什么都不说 —— 关了自动运行、或在下游钉住时敲数改参数，
 *  看着像改了没反应。跑着的时候不说：马上就有新结果，预览一次次重跑时它也会一闪一闪。 */
export function StaleBadge({
  nodeId,
  onRunToNode,
}: {
  nodeId: string;
  onRunToNode?: ((nodeId: string) => void) | undefined;
}) {
  const stale = useNodeStale(nodeId);
  const running = useExecutionStore((s) => s.runStatus === "running");
  const canRun = useManifestStore((s) => s.transportKind !== "static");
  if (!stale || running) return null;
  return (
    <div className="viewer__stale" data-testid="viewer-stale">
      <span>参数改过了 · 画面是上一次的结果</span>
      {onRunToNode && canRun && (
        <button
          type="button"
          data-testid="viewer-stale-run"
          title="算出这个节点（缺结果或过时的上游一并算，下游不动），与节点右键「运行到此节点」相同"
          onClick={() => onRunToNode(nodeId)}
        >
          ▶ 运行到此节点
        </button>
      )}
    </div>
  );
}

export function ViewerStatus({
  text,
  corner = false,
  nodeId = null,
  onRunToNode,
}: {
  text: string;
  /** 画面上还有东西（重跑中的上一片云、拖框的底图）：缩到角上，不给动作。 */
  corner?: boolean;
  /** 文字说的是这个节点（当前层）的运行状态时给，据此配动作。没有点云输出、取不到图这类提示不给。 */
  nodeId?: string | null;
  onRunToNode?: ((nodeId: string) => void) | undefined;
}) {
  const path = useUiStore((s) => s.path);
  const doc = useGraphStore((s) => s.doc);
  const target = corner ? null : nodeId;
  const exec = useExecutionStore((s) => (target ? aggregatedNodes(path, s.nodes).get(target) : undefined));
  const running = useExecutionStore((s) => s.runStatus === "running");
  const canRun = useManifestStore((s) => s.transportKind !== "static");
  const baseOps = useManifestStore((s) => s.operatorsById);
  // 连带没执行的：沿入边往上找离它最近的出错节点；这一层找不到（子图的入口不是边）就从子图节点那里往外一层接着找。
  // 结果是「第几层\n那一层的本地 id」，空串 = 哪一层都没找到。按字符串订阅，进度事件不让它算出新东西
  const culprit = useExecutionStore((s) => {
    if (!target) return null;
    const me = aggregatedNodes(path, s.nodes).get(target);
    if (me?.state !== "cancelled" || me.errors[0]?.code !== "upstream_failed") return null;
    const c = culpritOf(doc, path, target, s.nodes);
    return c ? `${c.path.length}\n${c.id}` : "";
  });

  const state = exec?.state;
  const first = exec?.errors[0];
  let detail: string | null = null;
  let action: React.ReactNode = null;
  if (target && state === "error") {
    detail = first?.message ?? null;
    action = (
      <button type="button" data-testid="viewer-reveal-error" onClick={() => revealNodeError(path, target)}>
        {first?.paramPath && !exec?.errorSource ? `定位到参数 ${first.paramPath}` : "定位到出错的地方"}
      </button>
    );
  } else if (target && culprit !== null) {
    const [depth, hit] = culprit ? culprit.split("\n") : [];
    const at = depth !== undefined ? path.slice(0, Number(depth)) : path;
    const name = hit ? nodeLabel(levelOf(doc, at), hit, augmentOperators(baseOps, doc.subgraphs)) : null;
    detail = name ? `出错的是上游的「${name}」` : (first?.message ?? null);
    action = (
      <button
        type="button"
        data-testid="viewer-reveal-upstream"
        onClick={() => (hit ? revealNodeError(at, hit) : revealError(0))}
      >
        定位到出错的节点
      </button>
    );
  } else if (target && onRunToNode && canRun && !running && state !== "done" && state !== "skipped" && state !== "running") {
    action = (
      <>
        <button
          type="button"
          data-testid="viewer-run-here"
          title="算出这个节点（缺结果或过时的上游一并算，下游不动），与节点右键「运行到此节点」相同"
          onClick={() => onRunToNode(target)}
        >
          ▶ 运行到此节点
        </button>
        <span className="viewer__empty-hint">或按 {keyHint("run")} 运行整张图</span>
      </>
    );
  }

  return (
    <div className={`viewer__empty${corner ? " viewer__empty--corner" : ""}`} data-testid="viewer-empty">
      <span data-testid="viewer3d-status">{text}</span>
      {detail && (
        <span className="viewer__empty-detail" data-testid="viewer-status-detail">
          {detail}
        </span>
      )}
      {action && <div className="viewer__empty-actions">{action}</div>}
    </div>
  );
}
