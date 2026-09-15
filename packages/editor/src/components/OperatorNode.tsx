// 画布上的算子节点。按 op id 现查 manifest，不把算子描述塞进节点 data ——
// 这是热重载的前提：推一份新 manifest 进 store，节点外观自动跟着变。

import { Handle, Position, type NodeProps } from "@xyflow/react";
import { memo, useEffect, useRef, useState } from "react";

import { augmentOperators } from "../lib/subgraph";
import { ANY } from "../lib/typecheck";
import type { OperatorNodeData } from "../lib/mapping";
import { useNodeStale } from "../store/cache";
import { useNodeExecution } from "../store/execution";
import { useGraphStore } from "../store/graph";
import { useManifestStore } from "../store/manifest";
import { useUiStore } from "../store/ui";
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
  nodeId: string;
  port: Port;
  side: "input" | "output";
  index: number;
  /** Any 端口推导出的实际类型，null = 还推不出来（E6）。 */
  anyType: string | null;
}

function PortHandle({ nodeId, port, side, index, anyType }: PortHandleProps) {
  const type = port.type === ANY ? (anyType ?? ANY) : port.type;
  const color = useManifestStore((s) => s.typesByName.get(type)?.color ?? "#6b7280");
  // 拖线中的兼容性可视化（交互清单 P1 #20）：能落的高亮，不能落的置灰。
  const verdict = useUiStore((s) => {
    if (!s.pendingFrom) return "";
    const key = `${nodeId}:${port.name}`;
    if (s.pendingFrom.side === side) return "";
    return s.compatiblePorts.has(key) ? "compatible" : "incompatible";
  });
  const isInput = side === "input";
  const optional = isInput && port.required === false;
  // 两条端口级执行语义（ADR-0016）。用角标而不是换颜色：颜色是类型的语言。
  const acceptsError = isInput && port.acceptsError === true;
  const lazy = isInput && port.lazy === true;
  const marks = [acceptsError ? "接受上游的错误值" : "", lazy ? "惰性：被 demand 时才调度" : ""]
    .filter(Boolean)
    .join("；");

  return (
    <div
      className={`node-port node-port--${side}${verdict ? ` node-port--${verdict}` : ""}`}
      style={{ ["--i" as string]: index }}
      data-port-verdict={verdict || undefined}
      data-port-accepts-error={acceptsError ? "1" : undefined}
      data-port-lazy={lazy ? "1" : undefined}
      data-testid={`port-${nodeId}-${port.name}`}
    >
      <Handle
        id={port.name}
        type={isInput ? "target" : "source"}
        position={isInput ? Position.Left : Position.Right}
        className="node-port__handle"
        style={{ background: color, borderColor: color }}
      />
      <span
        className="node-port__label"
        title={`${type}${port.doc ? " — " + port.doc : ""}${marks ? ` (${marks})` : ""}`}
      >
        {port.label || port.name}
        {optional && <em className="node-port__opt">?</em>}
        {acceptsError && <em className="node-port__mark node-port__mark--err">!</em>}
        {lazy && <em className="node-port__mark node-port__mark--lazy">~</em>}
      </span>
    </div>
  );
}

/** 双击标题就地改名（交互清单 P1 #25）。空串 = 回到 manifest 的 label。 */
function TitleEditor({ id, initial, onDone }: { id: string; initial: string; onDone: () => void }) {
  const [text, setText] = useState(initial);
  const input = useRef<HTMLInputElement>(null);

  useEffect(() => {
    input.current?.focus();
    input.current?.select();
  }, []);

  const commit = () => {
    useGraphStore.getState().renameNode(id, text.trim() ? text : null);
    onDone();
  };

  return (
    <input
      ref={input}
      className="node__rename"
      data-testid={`node-rename-${id}`}
      value={text}
      spellCheck={false}
      onChange={(e) => setText(e.target.value)}
      onBlur={commit}
      onKeyDown={(e) => {
        e.stopPropagation();
        if (e.key === "Enter") commit();
        if (e.key === "Escape") onDone();
      }}
      onDoubleClick={(e) => e.stopPropagation()}
    />
  );
}

function OperatorNodeImpl({ id, data, selected }: NodeProps) {
  const { opId, title, collapsed, bypass, anyType, subgraphId, library } =
    data as OperatorNodeData;
  const base = useManifestStore((s) => s.operatorsById);
  const subgraphs = useGraphStore((s) => s.doc.subgraphs);
  const op = augmentOperators(base, subgraphs).get(opId);
  // 执行状态从独立的 store 现查（P0 #14）：放进节点 data 的话，
  // 每来一条事件就要重建整个节点数组，几十个节点的图会肉眼可见地卡。
  const exec = useNodeExecution(id);
  // 精确到节点的 stale（交互清单 P1 #23）：判定在 C++，这里只读结论（ADR-0007）
  const stale = useNodeStale(id);
  const [renaming, setRenaming] = useState(false);

  // 算子在当前 core 里不存在：可能是打开了别人存的图，也可能是热重载删掉了它。
  // 必须显式画出来 —— 静默渲染成空节点会让人以为图坏了（1.5）。
  if (!op) {
    return (
      <div
        className={`node node--missing${selected ? " is-selected" : ""}`}
        data-node-state="missing"
        data-testid={`node-${id}`}
      >
        <div className="node__head">算子缺失</div>
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
  // 只被惰性端口依赖、这次没被 demand（ADR-0016）。画成半透明，与「命中缓存」区分开。
  const notDemanded = exec?.stats?.reason === "not_demanded";
  const classes = [
    "node",
    selected ? "is-selected" : "",
    state !== "idle" ? `node--${state}` : "",
    // stale 是虚线框，不是换颜色：颜色已经被状态占满了，再加一种就没人分得清
    stale ? "is-stale" : "",
    // 静音整体半透明加斜纹，一眼看得出这个节点这次不算
    bypass ? "is-bypassed" : "",
    notDemanded ? "is-not-demanded" : "",
    subgraphId || library ? "node--sub" : "",
  ]
    .filter(Boolean)
    .join(" ");

  const cached = exec?.stats?.cached === true;
  // 输出能不能取，认 outputsAvailable 而不是认 state：skipped 既可能是「算过了，输出照样在」
  // 也可能是「这一支根本没被需要，什么都没有」。老 core 不带这个字段，按 not_demanded 兜底。
  const outputsAvailable = exec?.stats?.outputsAvailable ?? !notDemanded;
  const skipLabel = !outputsAvailable ? "未被需要" : cached ? "已缓存" : "静音";
  const skipReason = !outputsAvailable
    ? "未被需要：这一支这次没被 demand，没有输出可取"
    : cached
      ? "已缓存：命中缓存未重算，输出可取"
      : bypass
        ? "已静音：输入直接透传，输出可取"
        : "输出可取";

  return (
    <div
      className={classes}
      data-node-state={state}
      data-stale={stale ? "1" : "0"}
      data-bypass={bypass ? "1" : "0"}
      data-not-demanded={notDemanded ? "1" : "0"}
      data-subgraph={subgraphId ?? undefined}
      data-library={library ? "1" : undefined}
      data-testid={`node-${id}`}
    >
      <div
        className="node__head"
        title={errorText ?? op.doc}
        onDoubleClick={(e) => {
          e.stopPropagation();
          setRenaming(true);
        }}
      >
        {renaming ? (
          <TitleEditor id={id} initial={title ?? op.label} onDone={() => setRenaming(false)} />
        ) : (
          <span className="node__title">{title ?? op.label}</span>
        )}
        {(subgraphId || library) && (
          <span
            className="node__badge node__badge--sub"
            data-testid={`node-subbadge-${id}`}
            title={subgraphId ? "子图：双击进入" : "库算子：右键可展开为内联子图"}
          >
            {subgraphId ? "⧉" : "L"}
          </span>
        )}
        {bypass && <span className="node__badge node__badge--mute" title="已静音 (Ctrl+M)">M</span>}
        {state === "running" && exec?.progress != null && (
          <span className="node__progress" style={{ ["--p" as string]: exec.progress }} />
        )}
      </div>

      {!collapsed && (
        <div className="node__body" style={{ ["--rows" as string]: rows }}>
          <div className="node__col node__col--in">
            {op.inputs.map((p, i) => (
              <PortHandle
                key={p.name}
                nodeId={id}
                port={p}
                side="input"
                index={i}
                anyType={anyType}
              />
            ))}
          </div>
          <div className="node__col node__col--out">
            {op.outputs.map((p, i) => (
              <PortHandle
                key={p.name}
                nodeId={id}
                port={p}
                side="output"
                index={i}
                anyType={anyType}
              />
            ))}
          </div>
        </div>
      )}

      {/* 状态条：点数与耗时。没跑过时整行不渲染，节点保持原来的高度。 */}
      {state !== "idle" && (
        <div className="node__stats" data-testid={`node-stats-${id}`}>
          <span className={`node__dot node__dot--${state}`} />
          {state === "skipped" && (
            <span
              className="node__skip"
              data-testid={`node-skip-${id}`}
              data-outputs-available={outputsAvailable ? "1" : "0"}
              title={skipReason}
            >
              {skipLabel}
            </span>
          )}
          {exec?.stats?.elementCount != null && (
            <span className="node__count">{formatCount(exec.stats.elementCount)}</span>
          )}
          {exec?.children && (
            <span className="node__count" data-testid={`node-children-${id}`}>
              {exec.children.finished}/{exec.children.total}
            </span>
          )}
          {exec?.durationMs != null && (
            <span className="node__time">{formatDuration(exec.durationMs)}</span>
          )}
          {errorText && <span className="node__err" title={errorText}>{errorText}</span>}
        </div>
      )}

      {/* 折叠时只显示标题与**已连线**的端口，其余收起来（交互清单 P1 #25）。
          端口本身必须还在，否则已有连线会掉。 */}
      {collapsed && (
        <div className="node__collapsed" data-testid={`node-collapsed-${id}`}>
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
