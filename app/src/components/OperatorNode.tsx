//
// 画布上的算子节点。
//
// **按 op id 现查 manifest，不接受把算子描述塞进节点 data。**
// 这是 M3 热重载的前提：C++ 重编后推一份新 manifest 进 store，
// 所有节点的外观和端口自动跟着变，当前打开的图不需要重置。
//

import { Handle, Position, type NodeProps } from "@xyflow/react";
import { memo } from "react";

import type { OperatorNodeData } from "../lib/mapping";
import { useExecutionStore, useNodeExecution } from "../store/execution";
import { useManifestStore } from "../store/manifest";
import type { Port } from "../types/manifest";

/** 「12.3 万点」比「123456」好读得多，而节点上的空间只有一行。 */
function formatCount(n: number): string {
  if (n >= 100_000_000) return `${(n / 100_000_000).toFixed(2)} 亿`;
  if (n >= 10_000) return `${(n / 10_000).toFixed(1)} 万`;
  return String(n);
}

function formatDuration(ms: number): string {
  if (ms >= 1000) return `${(ms / 1000).toFixed(2)}s`;
  return `${Math.round(ms)}ms`;
}

interface PortHandleProps {
  port: Port;
  side: "input" | "output";
  index: number;
}

function PortHandle({ port, side, index }: PortHandleProps) {
  const color = useManifestStore((s) => s.typesByName.get(port.type)?.color ?? "#6b7280");
  const isInput = side === "input";
  const optional = isInput && port.required === false;

  return (
    <div className={`node-port node-port--${side}`} style={{ ["--i" as string]: index }}>
      <Handle
        id={port.name}
        type={isInput ? "target" : "source"}
        position={isInput ? Position.Left : Position.Right}
        className="node-port__handle"
        style={{ background: color, borderColor: color }}
      />
      <span className="node-port__label" title={`${port.type}${port.doc ? " — " + port.doc : ""}`}>
        {port.label || port.name}
        {optional && <em className="node-port__opt">?</em>}
      </span>
    </div>
  );
}

function OperatorNodeImpl({ id, data, selected }: NodeProps) {
  const { opId, title, collapsed } = data as OperatorNodeData;
  const op = useManifestStore((s) => s.operatorsById.get(opId));
  // 执行状态从**独立的 store** 现查（交互清单 P0 #14）。它不在 GraphDoc 里，
  // 也不在节点 data 里 —— 否则每来一条事件就要重建整个节点数组，几十个节点
  // 的图会肉眼可见地卡。
  const exec = useNodeExecution(id);
  const stale = useExecutionStore((s) => s.stale);

  // 算子在当前 core 里不存在：可能是打开了别人存的图，或者算子被删/改名了。
  // 必须显式画出来 —— 静默渲染成空节点会让人以为图坏了。
  if (!op) {
    return (
      <div className={`node node--missing${selected ? " is-selected" : ""}`}>
        <div className="node__head">未知算子</div>
        <div className="node__missing-body">
          <code>{opId}</code>
          <span>当前 core 未注册</span>
        </div>
      </div>
    );
  }

  const rows = Math.max(op.inputs.length, op.outputs.length);
  const state = exec?.state ?? "idle";
  const errorText = exec?.errors[0]?.message;
  const classes = [
    "node",
    selected ? "is-selected" : "",
    state !== "idle" ? `node--${state}` : "",
    // stale 是整体降不透明度，不是换颜色：颜色已经被状态占满了，
    // 再加一种色就没人分得清了。
    stale && state !== "idle" ? "is-stale" : "",
  ]
    .filter(Boolean)
    .join(" ");

  return (
    <div className={classes} data-node-state={state} data-testid={`node-${id}`}>
      <div className="node__head" title={errorText ?? op.doc}>
        <span className="node__title">{title ?? op.label}</span>
        {state === "running" && exec?.progress != null && (
          <span className="node__progress" style={{ ["--p" as string]: exec.progress }} />
        )}
      </div>

      {!collapsed && (
        <div className="node__body" style={{ ["--rows" as string]: rows }}>
          <div className="node__col node__col--in">
            {op.inputs.map((p, i) => (
              <PortHandle key={p.name} port={p} side="input" index={i} />
            ))}
          </div>
          <div className="node__col node__col--out">
            {op.outputs.map((p, i) => (
              <PortHandle key={p.name} port={p} side="output" index={i} />
            ))}
          </div>
        </div>
      )}

      {/* 状态条：点数与耗时。没跑过时整行不渲染，节点保持原来的高度。 */}
      {state !== "idle" && (
        <div className="node__stats" data-testid={`node-stats-${id}`}>
          <span className={`node__dot node__dot--${state}`} />
          {exec?.stats?.elementCount != null && (
            <span className="node__count">{formatCount(exec.stats.elementCount)}</span>
          )}
          {exec?.durationMs != null && (
            <span className="node__time">{formatDuration(exec.durationMs)}</span>
          )}
          {errorText && <span className="node__err" title={errorText}>{errorText}</span>}
        </div>
      )}

      {/* 折叠时端口仍要存在，否则已有连线会掉。只是收到标题两侧。 */}
      {collapsed && (
        <div className="node__collapsed">
          {op.inputs.map((p) => (
            <Handle
              key={`in-${p.name}`}
              id={p.name}
              type="target"
              position={Position.Left}
              className="node-port__handle node-port__handle--collapsed"
            />
          ))}
          {op.outputs.map((p) => (
            <Handle
              key={`out-${p.name}`}
              id={p.name}
              type="source"
              position={Position.Right}
              className="node-port__handle node-port__handle--collapsed"
            />
          ))}
        </div>
      )}
    </div>
  );
}

export const OperatorNode = memo(OperatorNodeImpl);
