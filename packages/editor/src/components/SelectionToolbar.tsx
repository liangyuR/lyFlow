import { memo, type KeyboardEvent } from "react";
import { NodeToolbar, Position } from "@xyflow/react";

import "../styles.selection.css";

export interface SelectionToolbarProps {
  selectedNodeIds: string[];
  visible: boolean;
  disabled: boolean;
  onRun: () => void;
  onLayout: () => void;
  onClear: () => void;
}

function SelectionToolbarView({ selectedNodeIds, visible, disabled, onRun, onLayout, onClear }: SelectionToolbarProps) {
  if (!visible || selectedNodeIds.length < 2) return null;

  const onKeyDown = (event: KeyboardEvent<HTMLDivElement>) => {
    if (event.key === "Escape") {
      event.preventDefault();
      event.stopPropagation();
      onClear();
      return;
    }
    if (!["ArrowLeft", "ArrowRight", "Home", "End"].includes(event.key)) return;
    const buttons = [...event.currentTarget.querySelectorAll<HTMLButtonElement>("button:not(:disabled)")];
    if (buttons.length === 0) return;
    const current = buttons.indexOf(document.activeElement as HTMLButtonElement);
    const index = event.key === "Home" ? 0 : event.key === "End" ? buttons.length - 1
      : (current + (event.key === "ArrowRight" ? 1 : -1) + buttons.length) % buttons.length;
    event.preventDefault();
    event.stopPropagation();
    buttons[index]?.focus();
  };

  return (
    <NodeToolbar
      nodeId={selectedNodeIds}
      isVisible
      position={Position.Top}
      offset={16}
      className="selection-toolbar qs-popover nodrag nopan nowheel"
      role="toolbar"
      aria-label="选中节点操作"
      data-testid="selection-toolbar"
      onPointerDown={(event) => event.stopPropagation()}
      onDoubleClick={(event) => event.stopPropagation()}
      onKeyDown={onKeyDown}
    >
      <span className="selection-toolbar__count" data-testid="selection-count">
        已选 <strong>{selectedNodeIds.length}</strong> 个节点
      </span>
      <button
        type="button"
        className="qs-btn sm with-leading"
        data-testid="selection-run"
        disabled={disabled}
        title="运行选中节点及所需上游，不运行未选中的下游"
        onClick={onRun}
      >
        <svg className="qs-ic" viewBox="0 0 24 24" aria-hidden="true"><path d="m8 5 11 7-11 7V5Z" /></svg>
        运行选中
      </button>
      <span className="selection-toolbar__divider" aria-hidden="true" />
      <button
        type="button"
        className="qs-btn icon sm"
        data-testid="selection-layout"
        aria-label="整理布局"
        title="整理布局"
        onClick={onLayout}
      >
        <svg className="qs-ic" viewBox="0 0 24 24" aria-hidden="true"><rect x="3" y="3" width="7" height="7" rx="2" /><rect x="14" y="14" width="7" height="7" rx="2" /><path d="M14 6h7M17.5 3v7M3 17.5h7M6.5 14v7" /></svg>
      </button>
      <button
        type="button"
        className="qs-btn icon sm"
        data-testid="selection-clear"
        aria-label="取消选择"
        title="取消选择 (Esc)"
        onClick={onClear}
      >
        <svg className="qs-ic" viewBox="0 0 24 24" aria-hidden="true"><path d="m6 6 12 12M18 6 6 18" /></svg>
      </button>
    </NodeToolbar>
  );
}

export const SelectionToolbar = memo(SelectionToolbarView);
