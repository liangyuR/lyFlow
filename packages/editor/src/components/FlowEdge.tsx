// 画布上唯一的连线组件（docs/motion-plan.md E1）：替换 React Flow 的 default 边，
// 原来的 LazyEdge 并进来按 data.lazy 决定虚线与 tooltip。hover、关联高亮、数据流动、
// 连线生长都在这里 —— 默认边拿不到执行状态，也挂不上这些类。

import { getBezierPath, type EdgeProps } from "@xyflow/react";
import { motion, type MotionStyle } from "motion/react";
import { useState, type CSSProperties } from "react";

import type { LyEdge } from "../lib/mapping";
import { isEntering, TRANSITION, useMotionEnabled } from "../lib/motion";
import { useNodeState } from "../store/execution";
import { useUiStore } from "../store/ui";

/** 惰性边的说明。SVG 的 <title> 作为 <g> 的子元素时，hover 到可见描边或旁边的
 *  透明命中路径都能触发，不用另起一层 DOM。 */
const LAZY_TITLE = "惰性：主路径成功时不跑";

function withoutDash(style: CSSProperties): CSSProperties {
  const { strokeDasharray: _dash, ...rest } = style;
  return rest;
}

export function FlowEdge({
  id,
  source,
  target,
  sourceX,
  sourceY,
  sourcePosition,
  targetX,
  targetY,
  targetPosition,
  style,
  markerStart,
  markerEnd,
  // 默认值与 React Flow 的 BaseEdge 一致：边对象上不写 interactionWidth 时它按 20 画命中路径
  interactionWidth = 20,
  data,
}: EdgeProps<LyEdge>) {
  const [path] = getBezierPath({
    sourceX,
    sourceY,
    sourcePosition,
    targetX,
    targetY,
    targetPosition,
  });
  const lazy = data?.lazy === true;
  const motionOn = useMotionEnabled();
  // 生长只给画布差分认定的新边（N1/E2）。虚拟化下平移进视口重新挂载的边问到的是 false。
  const [growing, setGrowing] = useState(() => motionOn && isEntering("edge", id));

  // 数据流动（E3）：目标节点 running 的那段时间，数据正被下游消费。只订阅那一个状态串。
  const flowing = useNodeState(target) === "running";
  // 节点 hover 高亮关联边（H2）。拖连线、拖节点、框选期间不淡化。
  const relation = useUiStore((s) => {
    const hover = s.hoverNodeId;
    if (!hover || s.pendingFrom || s.hoverPaused) return "";
    return hover === source || hover === target ? "related" : "dimmed";
  });

  const color = typeof style?.stroke === "string" ? style.stroke : undefined;
  // 生长期间画实线：dasharray 被 pathLength 占用，惰性边长完再恢复 6 4（E2）
  const growStyle = style && lazy ? withoutDash(style) : style;
  const classes = [
    "ly-edge",
    flowing ? "is-flowing" : "",
    relation ? `is-${relation}` : "",
    lazy ? "is-lazy" : "",
  ]
    .filter(Boolean)
    .join(" ");

  return (
    <g
      className={classes}
      data-flowing={flowing ? "1" : undefined}
      data-growing={growing ? "1" : undefined}
      data-relation={relation || undefined}
      // hover 光晕要用边自己的颜色（H3）：经变量交给 CSS
      style={color ? ({ ["--lyflow-edge-color" as string]: color } as CSSProperties) : undefined}
    >
      {lazy && <title>{LAZY_TITLE}</title>}
      {growing ? (
        <motion.path
          id={id}
          d={path}
          fill="none"
          className="react-flow__edge-path"
          // exactOptionalPropertyTypes 下 motion 的 style 不收显式 undefined，只能按有无展开
          {...(growStyle ? { style: growStyle as MotionStyle } : {})}
          initial={{ pathLength: 0 }}
          animate={{ pathLength: 1 }}
          transition={TRANSITION.base}
          // 长完换回普通 path：motion 留在元素上的 pathLength / dasharray 一并消失
          onAnimationComplete={() => setGrowing(false)}
        />
      ) : (
        <path
          id={id}
          d={path}
          fill="none"
          className="react-flow__edge-path"
          style={style}
          markerStart={markerStart}
          markerEnd={markerEnd}
        />
      )}
      {/* 流动层叠在可见描边上，同色半透明；关动效时它留着不动，仍然表示「正在流」（A4） */}
      {flowing && (
        <path
          d={path}
          fill="none"
          className="ly-edge__flow"
          style={color ? { stroke: color } : undefined}
        />
      )}
      {/* 命中路径。类名与 React Flow 的 BaseEdge 一致：e2e 与 peek 靠它找点 */}
      {interactionWidth ? (
        <path
          d={path}
          fill="none"
          strokeOpacity={0}
          strokeWidth={interactionWidth}
          className="react-flow__edge-interaction"
        />
      ) : null}
    </g>
  );
}
