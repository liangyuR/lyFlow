// 标题栏右端的「只运行此节点」小圆圈（docs/node-run-plan.md A 方案）。只重算这一个节点：
// 上游用已有结果，下游不动。判据、状态与动作都在 lib/nodeRun.ts，右键菜单用的是同一套。
//
// 按钮在 .node__head 里，不含端口（motion-plan A5 / A6）：hover 的放大画在它自己的 SVG 上，
// 碰不到 React Flow 的端口量测。

import { memo, useMemo, type SyntheticEvent } from "react";

import {
  isOwnRun,
  missingUpstream,
  nodeLabel,
  nodeRunState,
  nodeRunTitle,
  runUpstreamOf,
  toggleNodeRun,
} from "../lib/nodeRun";
import { augmentOperators, fullId, levelOf } from "../lib/subgraph";
import { useStaleNodeIds } from "../store/cache";
import { aggregatedNodes, useExecutionStore, type NodeExecution } from "../store/execution";
import { useGraphStore } from "../store/graph";
import { useManifestStore } from "../store/manifest";
import { useUiStore } from "../store/ui";
import type { OperatorDesc } from "../types/manifest";

/** 进度环的几何：视觉 14 px，描边 1.5 px 画在圆周内侧（U2）。 */
const R = 6.25;

interface NodeRunButtonProps {
  id: string;
  op: OperatorDesc;
  exec: NodeExecution | undefined;
}

/** 按下、点击、双击都不许冒泡（U1）：否则会选中节点、开始拖动、或进入改名。 */
const swallow = (e: SyntheticEvent) => e.stopPropagation();

function NodeRunButtonImpl({ id, op, exec }: NodeRunButtonProps) {
  const path = useUiStore((s) => s.path);
  const full = fullId(path, id);
  // 上游清单按字符串订阅：doc 的别处变了（拖别的节点、改参数）不让这个按钮重渲
  const upstreamKey = useGraphStore((s) => runUpstreamOf(levelOf(s.doc, path), id, op).join("\n"));
  const upstream = useMemo(() => (upstreamKey ? upstreamKey.split("\n") : []), [upstreamKey]);
  const stale = useStaleNodeIds();
  const missingKey = useExecutionStore((s) =>
    missingUpstream(upstream, aggregatedNodes(path, s.nodes), stale).join("\n"),
  );
  const missing = useMemo(() => (missingKey ? missingKey.split("\n") : []), [missingKey]);
  const own = useExecutionStore((s) => s.runStatus === "running" && isOwnRun(s.isolate, full));
  const baseOps = useManifestStore((s) => s.operatorsById);
  const namesKey = useGraphStore((s) => {
    if (missing.length === 0) return "";
    const ops = augmentOperators(baseOps, s.doc.subgraphs);
    const level = levelOf(s.doc, path);
    return missing.map((m) => nodeLabel(level, m, ops)).join("\n");
  });

  const state = nodeRunState(exec, missing, own);
  const title = nodeRunTitle(state, namesKey ? namesKey.split("\n") : [], own);
  // 有进度就画弧长 = progress；算子不报进度（或刚进 running 还是 0）时是转圈的 3/4 弧
  const progress = state === "running" && exec?.progress != null && exec.progress > 0 ? exec.progress : null;

  return (
    <button
      type="button"
      className="node-run nodrag nopan"
      data-testid={`node-run-${id}`}
      data-run-state={state}
      data-run-own={own ? "1" : undefined}
      data-run-reason={state === "disabled" ? missing.join(",") : undefined}
      data-run-progress={progress ?? undefined}
      aria-label={title}
      aria-disabled={state === "disabled" ? true : undefined}
      title={title}
      onPointerDown={swallow}
      onMouseDown={swallow}
      onDoubleClick={swallow}
      onClick={(e) => {
        e.stopPropagation();
        if (state === "disabled") return;
        void toggleNodeRun(id);
      }}
    >
      <svg className="node-run__svg" viewBox="0 0 14 14" width="14" height="14" aria-hidden="true">
        <circle className="node-run__ring" cx="7" cy="7" r={R} />
        {state === "running" && (
          <circle
            className={`node-run__arc${progress === null ? " node-run__arc--spin" : ""}`}
            cx="7"
            cy="7"
            r={R}
            pathLength={1}
            strokeDasharray={progress === null ? "0.75 1" : `${progress} 1`}
          />
        )}
        {own ? (
          <rect className="node-run__stop" x="4.75" y="4.75" width="4.5" height="4.5" rx="0.6" />
        ) : (
          <path className="node-run__play" d="M5.6 4.4 L9.8 7 L5.6 9.6 Z" />
        )}
      </svg>
    </button>
  );
}

export const NodeRunButton = memo(NodeRunButtonImpl);
