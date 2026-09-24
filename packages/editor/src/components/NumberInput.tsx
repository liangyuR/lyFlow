// 数字框与滑块：参数表单里所有数字（int / float、向量分量、transform、curve 的控制点）共用这一个。
// 撤销粒度：打字失焦/回车才提交（一次编辑一条撤销），横向拖动与滑块用 begin()/commit() 包住一整段。

import { useEffect, useRef, useState } from "react";

import { beginPreview, endPreview, schedulePreview } from "../lib/preview";
import { useGraphStore } from "../store/graph";
import type { Param } from "../types/manifest";

/** 拖过这么多像素才算一格。太小手抖就改值，太大又拖不动。 */
const DRAG_PX_PER_STEP = 4;
/** 先动这么多像素才进入拖动，否则普通点击就没法聚焦输入框打字了。 */
const DRAG_THRESHOLD_PX = 3;

interface DragState {
  id: number;
  startX: number;
  lastX: number;
  /** 未量化的累计值。量化只作用于写出去的那一份，否则慢拖会永远走不动。 */
  acc: number;
  active: boolean;
}

/** 一格的大小：manifest 的 step 优先，其次整数 1、soft 范围的 1/200，最后 0.01。 */
export function dragStepOf(param: Param, integer: boolean): number {
  if (param.step !== undefined && param.step > 0) return param.step;
  if (integer) return 1;
  const lo = param.softMin ?? param.min;
  const hi = param.softMax ?? param.max;
  if (lo !== undefined && hi !== undefined && Number.isFinite(lo) && Number.isFinite(hi) && hi > lo) {
    return (hi - lo) / 200;
  }
  return 0.01;
}

/** 吸到 step 的整数倍，并抹掉二进制浮点的尾巴（0.30000000000000004）。 */
function quantize(v: number, q: number): number {
  if (!(q > 0)) return v;
  return Number.parseFloat((Math.round(v / q) * q).toPrecision(12));
}

/** 收尾一次拖动。卸载路径也要走这里，否则事务悬着、body 上的类留着。 */
function finishDrag(d: DragState | null, nodeId?: string): void {
  if (!d?.active) return;
  d.active = false;
  document.body.classList.remove("param-dragging");
  useGraphStore.getState().commit("拖动参数");
  endPreview(nodeId);
}

export interface NumberInputProps {
  value: number;
  disabled: boolean;
  integer: boolean;
  min?: number | undefined;
  max?: number | undefined;
  step?: number | undefined;
  dragStep: number;
  dragName?: string | undefined;
  nodeId?: string | undefined;
  onCommit: (v: number) => void;
}

export function NumberInput({
  value,
  disabled,
  integer,
  min,
  max,
  step,
  dragStep,
  dragName,
  nodeId,
  onCommit,
}: NumberInputProps) {
  const [text, setText] = useState(String(value));
  const [dragging, setDragging] = useState(false);
  const editing = useRef(false);
  const drag = useRef<DragState | null>(null);

  // 外部值变了（撤销、切换节点）且用户没在编辑时才同步，
  // 否则会在用户打字的中途把输入框内容抢走。
  useEffect(() => {
    if (!editing.current) setText(String(value));
  }, [value]);

  useEffect(() => () => finishDrag(drag.current, nodeId), [nodeId]);

  const clamp = (v: number) => {
    let n = v;
    if (min !== undefined) n = Math.max(n, min);
    if (max !== undefined) n = Math.min(n, max);
    return n;
  };

  const commit = () => {
    editing.current = false;
    const parsed = integer ? parseInt(text, 10) : parseFloat(text);
    if (Number.isNaN(parsed)) {
      setText(String(value)); // 输入非法，恢复原值而不是写入 NaN
      return;
    }
    const next = clamp(parsed);
    setText(String(next));
    if (next !== value) onCommit(next);
  };

  // 先抓住指针再判阈值：拖出输入框之外的那一段也要收得到
  const onPointerDown = (e: React.PointerEvent<HTMLInputElement>) => {
    if (disabled || e.button !== 0) return;
    e.currentTarget.setPointerCapture(e.pointerId);
    drag.current = { id: e.pointerId, startX: e.clientX, lastX: e.clientX, acc: value, active: false };
  };

  const onPointerMove = (e: React.PointerEvent<HTMLInputElement>) => {
    const d = drag.current;
    if (!d || d.id !== e.pointerId) return;
    if (!d.active) {
      if (Math.abs(e.clientX - d.startX) < DRAG_THRESHOLD_PX) return;
      d.active = true;
      d.lastX = e.clientX;
      d.acc = value;
      editing.current = false; // 拖动压过打字，接下来由它接管显示
      document.body.classList.add("param-dragging");
      setDragging(true);
      useGraphStore.getState().begin();
      if (nodeId) beginPreview(nodeId);
    }
    const mult = (e.shiftKey ? 10 : 1) * (e.altKey ? 0.1 : 1);
    d.acc = clamp(d.acc + ((e.clientX - d.lastX) / DRAG_PX_PER_STEP) * dragStep * mult);
    d.lastX = e.clientX;
    let next = clamp(quantize(d.acc, dragStep));
    if (integer) next = clamp(Math.round(next));
    setText(String(next));
    if (next !== value) {
      onCommit(next);
      if (nodeId) schedulePreview(nodeId);
    }
  };

  const onPointerEnd = (e: React.PointerEvent<HTMLInputElement>) => {
    const d = drag.current;
    if (!d || d.id !== e.pointerId) return;
    drag.current = null;
    if (e.currentTarget.hasPointerCapture(e.pointerId)) e.currentTarget.releasePointerCapture(e.pointerId);
    if (!d.active) return;
    finishDrag(d, nodeId);
    setDragging(false);
  };

  return (
    <input
      className={`ctl ctl--num${disabled ? "" : " is-draggable"}`}
      type="number"
      disabled={disabled}
      value={text}
      step={step ?? (integer ? 1 : "any")}
      data-testid={dragName ? `param-drag-${dragName}` : undefined}
      data-dragging={dragging ? "1" : undefined}
      onPointerDown={onPointerDown}
      onPointerMove={onPointerMove}
      onPointerUp={onPointerEnd}
      onPointerCancel={onPointerEnd}
      onFocus={() => (editing.current = true)}
      onChange={(e) => {
        editing.current = true;
        setText(e.target.value);
      }}
      onBlur={commit}
      onKeyDown={(e) => {
        if (e.key === "Enter") {
          e.currentTarget.blur();
        } else if (e.key === "Escape") {
          editing.current = false;
          setText(String(value));
          e.currentTarget.blur();
        }
      }}
    />
  );
}

/** 有 soft 范围时额外给一个滑块。拖动整段只记一条撤销。 */
export function Slider({
  value,
  min,
  max,
  step,
  disabled,
  nodeId,
  testId,
  onDrag,
}: {
  value: number;
  min: number;
  max: number;
  step: number | undefined;
  disabled: boolean;
  nodeId?: string | undefined;
  testId?: string | undefined;
  onDrag: (v: number) => void;
}) {
  return (
    <input
      className="ctl ctl--slider"
      type="range"
      data-testid={testId}
      disabled={disabled}
      min={min}
      max={max}
      step={step ?? (max - min) / 200}
      value={Math.min(Math.max(value, min), max)}
      onPointerDown={() => {
        useGraphStore.getState().begin();
        if (nodeId) beginPreview(nodeId);
      }}
      onPointerUp={() => {
        useGraphStore.getState().commit("拖动参数");
        endPreview(nodeId);
      }}
      onChange={(e) => {
        onDrag(parseFloat(e.target.value));
        if (nodeId) schedulePreview(nodeId);
      }}
    />
  );
}
