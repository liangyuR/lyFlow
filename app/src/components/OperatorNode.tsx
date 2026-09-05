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
import { useManifestStore } from "../store/manifest";
import type { Port } from "../types/manifest";

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

function OperatorNodeImpl({ data, selected }: NodeProps) {
  const { opId, title, collapsed } = data as OperatorNodeData;
  const op = useManifestStore((s) => s.operatorsById.get(opId));

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

  return (
    <div className={`node${selected ? " is-selected" : ""}`}>
      <div className="node__head" title={op.doc}>
        <span className="node__title">{title ?? op.label}</span>
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
