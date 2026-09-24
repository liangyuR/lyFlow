// 标题栏右端的运行小圆圈（docs/node-run-plan.md A 方案，§6 修订一）。单击 = 智能运行：本节点 +
// 缺结果或过时的上游，有缓存的复用，下游不动；Shift+单击 = 强制重算此节点。判据、状态与动作都在
// lib/nodeRun.ts，右键菜单用的是同一套。
//
// 按钮在 .node__head 里，不含端口（motion-plan A5 / A6）：hover 的放大画在它自己的 SVG 上，
// 碰不到 React Flow 的端口量测。

import { memo, useMemo, type SyntheticEvent } from "react";

import {
  ancestorsOf,
  isOwnRun,
  nodeLabel,
  nodeRunState,
  nodeRunTitle,
  toggleNodeRun,
  upstreamToRun,
  willCompute,
  type RunForecastInput,
} from "../lib/nodeRun";
import { augmentOperators, fullId, levelOf } from "../lib/subgraph";
import { useCacheStore, useStaleNodeIds } from "../store/cache";
import { aggregatedNodes, useExecutionStore, type NodeExecution } from "../store/execution";
import { useGraphStore } from "../store/graph";
import { useManifestStore } from "../store/manifest";
import { useUiStore } from "../store/ui";

/** 进度环的几何：视觉 14 px，描边 1.5 px 画在圆周内侧（U2）。 */
const R = 6.25;

interface NodeRunButtonProps {
  id: string;
  exec: NodeExecution | undefined;
  /** 本节点有编辑期校验 error（V3：唯一会让按钮置灰的情况）。 */
  invalid: boolean;
}

/** 按下、点击、双击都不许冒泡（U1）：否则会选中节点、开始拖动、或进入改名。 */
const swallow = (e: SyntheticEvent) => e.stopPropagation();

const splitKey = (key: string) => (key ? key.split("\n") : []);

function NodeRunButtonImpl({ id, exec, invalid }: NodeRunButtonProps) {
  const path = useUiStore((s) => s.path);
  const full = fullId(path, id);
  // 祖先清单按字符串订阅：doc 的别处变了（拖别的节点、改参数）不让这个按钮重渲
  const ancestorsKey = useGraphStore((s) => ancestorsOf(levelOf(s.doc, path), id).join("\n"));
  const ancestors = useMemo(() => splitKey(ancestorsKey), [ancestorsKey]);
  const stale = useStaleNodeIds();
  const plan = useCacheStore((s) => s.plan);
  const planUsable = useCacheStore((s) => !s.unavailable && s.plan.size > 0);
  // 预判（V5）同样按字符串订阅：每 50 ms 一条的进度不让它重算出新引用
  const forecastKey = useExecutionStore((s) => {
    const input: RunForecastInput = { path, plan, planUsable, execs: aggregatedNodes(path, s.nodes), stale };
    const up = upstreamToRun(input, ancestors);
    return `${willCompute(input, id) ? "1" : "0"}|${up.join("\n")}`;
  });
  const [selfKey, upstreamKey = ""] = forecastKey.split("|");
  const upstream = useMemo(() => splitKey(upstreamKey), [upstreamKey]);
  const upToDate = selfKey === "0" && upstream.length === 0;
  const own = useExecutionStore((s) => s.runStatus === "running" && isOwnRun(s.targets, full));
  const baseOps = useManifestStore((s) => s.operatorsById);
  const namesKey = useGraphStore((s) => {
    if (upstream.length === 0) return "";
    const ops = augmentOperators(baseOps, s.doc.subgraphs);
    const level = levelOf(s.doc, path);
    return upstream.map((u) => nodeLabel(level, u, ops)).join("\n");
  });

  const state = nodeRunState(exec, invalid, own);
  const title = nodeRunTitle({ state, own, upstream: splitKey(namesKey), upToDate });
  // 有进度就画弧长 = progress；算子不报进度（或刚进 running 还是 0）时是转圈的 3/4 弧
  const progress = state === "running" && exec?.progress != null && exec.progress > 0 ? exec.progress : null;

  return (
    <button
      type="button"
      className="node-run nodrag nopan"
      data-testid={`node-run-${id}`}
      data-run-state={state}
      data-run-own={own ? "1" : undefined}
      data-run-upstream={upstream.length > 0 ? upstream.join(",") : undefined}
      data-run-uptodate={upToDate ? "1" : undefined}
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
        void toggleNodeRun(id, e.shiftKey);
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
