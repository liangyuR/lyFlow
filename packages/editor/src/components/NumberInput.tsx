// 数字框与滑块：参数表单里所有数字（int / float、向量分量、transform、curve 的控制点）共用这一个。
// 撤销粒度：打字失焦/回车才提交（一次编辑一条撤销），横向拖动与滑块用 begin()/commit() 包住一整段。

import { useEffect, useRef, useState } from "react";

import { applyNumEdit, numEditNote, parseNumEdit, type NumEdit } from "../lib/numExpr";
import { beginPreview, endPreview, schedulePreview } from "../lib/preview";
import { rootOf } from "../lib/root";
import { useGraphStore } from "../store/graph";
import { useUiStore } from "../store/ui";

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
  /** 拖动时挂 lyflow-param-dragging 的那个编辑器根（不是 body）。 */
  host: HTMLElement | null;
  /** 这一段拖动的一格：开拖时按当时的值定下来，拖过 10 的整数次幂也不换档。 */
  step: number;
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
  d.host?.classList.remove("lyflow-param-dragging");
  useGraphStore.getState().commit("拖动参数");
  endPreview(nodeId);
}

export interface NumberInputProps {
  value: number;
  disabled: boolean;
  integer: boolean;
  min?: number | undefined;
  max?: number | undefined;
  /** 不再用（框是 type=text、能打算式了，浏览器的步进不在了）；留着只为调用处不用改。 */
  step?: number | undefined;
  /** 提示里怎么称呼它（越界夹住、看不懂的那句话）。 */
  label?: string | undefined;
  /** 打的是相对改法（*2、+=5……）时交给它：多选时每个节点按各自的值改。不给就按这个框的值改。 */
  onRelative?: ((edit: Extract<NumEdit, { kind: "rel" }>) => void) | undefined;
  /** 拖一格、按一下 ↑↓ 走多少。给函数时按当前值算（lib/params.ts 的 stepFor：没有范围的参数看值的量级）。 */
  dragStep: number | ((ref: number) => number);
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
  label,
  onRelative,
  dragStep,
  dragName,
  nodeId,
  onCommit,
}: NumberInputProps) {
  const [text, setText] = useState(String(value));
  /** 回车时打的字看不懂：框标红、焦点留着改，不提交也不恢复。 */
  const [invalid, setInvalid] = useState(false);
  const [dragging, setDragging] = useState(false);
  const editing = useRef(false);
  /** Esc 按下了：接着的那次 blur 撤回、不提交。setText 赶不上 —— blur 在同一个事件里同步触发，
   *  onBlur 拿到的还是这一帧的 text（打进去的那个），以前 Esc 就把它提交了。 */
  const cancelled = useRef(false);
  const drag = useRef<DragState | null>(null);

  // 外部值变了（撤销、切换节点）且用户没在编辑时才同步，
  // 否则会在用户打字的中途把输入框内容抢走。
  useEffect(() => {
    if (!editing.current) setText(String(value));
  }, [value]);

  useEffect(() => () => finishDrag(drag.current, nodeId), [nodeId]);

  const stepAt = (ref: number) => (typeof dragStep === "function" ? dragStep(ref) : dragStep);
  /** Shift ×10、Alt ×0.1 之后的一格。整数至少 1。 */
  const unitOf = (step: number, shift: boolean, alt: boolean) => {
    const u = step * (shift ? 10 : 1) * (alt ? 0.1 : 1);
    return integer ? Math.max(1, Math.round(u)) : u;
  };

  const clamp = (v: number) => {
    let n = v;
    if (min !== undefined) n = Math.max(n, min);
    if (max !== undefined) n = Math.min(n, max);
    return n;
  };

  const who = label ?? dragName ?? "这个数";

  const commit = () => {
    editing.current = false;
    setInvalid(false);
    if (cancelled.current) {
      cancelled.current = false;
      setText(String(value));
      return;
    }
    const edit = parseNumEdit(text);
    if (edit.kind === "empty") {
      setText(String(value));
      return;
    }
    if (edit.kind === "error") {
      // 看不懂：恢复原值、说一声（以前悄悄恢复，「1.5abc」还被当成 1.5 提交了）
      useUiStore.getState().showToast(`${who}：没看懂「${text}」（${edit.msg}），已恢复 ${value}`, "warn");
      setText(String(value));
      return;
    }
    if (edit.kind === "rel" && onRelative) {
      onRelative(edit);
      setText(String(value));
      return;
    }
    const r = applyNumEdit(edit, value, { integer, min, max });
    const note = numEditNote(who, [r], { min, max });
    if (note) useUiStore.getState().showToast(note, "warn");
    setText(String(r.value));
    if (r.value !== value) onCommit(r.value);
  };

  // 先抓住指针再判阈值：拖出输入框之外的那一段也要收得到
  const onPointerDown = (e: React.PointerEvent<HTMLInputElement>) => {
    if (disabled || e.button !== 0) return;
    e.currentTarget.setPointerCapture(e.pointerId);
    drag.current = {
      id: e.pointerId,
      startX: e.clientX,
      lastX: e.clientX,
      acc: value,
      active: false,
      host: rootOf(e.currentTarget),
      step: 0,
    };
  };

  const onPointerMove = (e: React.PointerEvent<HTMLInputElement>) => {
    const d = drag.current;
    if (!d || d.id !== e.pointerId) return;
    if (!d.active) {
      if (Math.abs(e.clientX - d.startX) < DRAG_THRESHOLD_PX) return;
      d.active = true;
      d.lastX = e.clientX;
      d.acc = value;
      d.step = stepAt(value);
      editing.current = false; // 拖动压过打字，接下来由它接管显示
      d.host?.classList.add("lyflow-param-dragging");
      setDragging(true);
      useGraphStore.getState().begin();
      if (nodeId) beginPreview(nodeId);
    }
    // 按实际的一格量化：以前 Alt 细调之后又按原步长吸回去，细调等于没调
    const unit = unitOf(d.step, e.shiftKey, e.altKey);
    d.acc = clamp(d.acc + ((e.clientX - d.lastX) / DRAG_PX_PER_STEP) * unit);
    d.lastX = e.clientX;
    let next = clamp(quantize(d.acc, unit));
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
    // 真拖过：焦点别留在框里。按下时浏览器把焦点给了它，拖完紧接着按 Ctrl+Z 就落进了输入框（浏览器对脚本写进去的值
    // 没有可撤的），拖过的那一下撤不掉。框里的字已经是拖到的值，失焦的提交是空操作。只点不拖照旧聚焦，接着打字
    e.currentTarget.blur();
  };

  return (
    <input
      className={`ctl ctl--num${disabled ? "" : " is-draggable"}`}
      // 能打算式与相对改法（0.01*2、*2、+=5）：type=number 不让打 * ( )
      type="text"
      spellCheck={false}
      autoComplete="off"
      disabled={disabled}
      value={text}
      title="可以打算式（0.01*2、(3+4)/2）或相对改法（*2、/2、+=5、-=0.5）；回车提交、Esc 撤回"
      data-testid={dragName ? `param-drag-${dragName}` : undefined}
      data-invalid={invalid ? "1" : undefined}
      aria-invalid={invalid || undefined}
      data-dragging={dragging ? "1" : undefined}
      onPointerDown={onPointerDown}
      onPointerMove={onPointerMove}
      onPointerUp={onPointerEnd}
      onPointerCancel={onPointerEnd}
      onFocus={() => (editing.current = true)}
      // 聚焦的数字框上滚滚轮：浏览器会一格一格改它的值（点过一下框、再滚面板，参数就悄悄变了）。
      // 先失焦（照常提交打了的字），这一下滚轮就去滚面板
      onWheel={(e) => {
        if (document.activeElement === e.currentTarget) e.currentTarget.blur();
      }}
      onChange={(e) => {
        editing.current = true;
        setInvalid(false);
        setText(e.target.value);
      }}
      onBlur={commit}
      onKeyDown={(e) => {
        // 输入法正在拼：回车、Esc、方向键都是它的
        if (e.nativeEvent.isComposing || e.keyCode === 229) return;
        if (e.key === "Enter") {
          // 看不懂：标红、焦点留着改（失焦才恢复）
          const edit = parseNumEdit(text);
          if (edit.kind === "error") {
            setInvalid(true);
            useUiStore.getState().showToast(`${who}：没看懂「${text}」（${edit.msg}），改一下再回车，Esc 撤回`, "warn");
            return;
          }
          e.currentTarget.blur();
        } else if (e.key === "Escape") {
          cancelled.current = true;
          e.currentTarget.blur();
        } else if (e.key === "ArrowUp" || e.key === "ArrowDown") {
          // 按这个参数的步长走一格（Shift ×10、Alt ×0.1，与拖动一样），只改框里的字，回车或失焦才提交。
          // 浏览器自己的步进在没声明 step 的浮点框上是 ±1：体素边长 0.01 按一下成了 1.01
          e.preventDefault();
          const typed = parseNumEdit(text);
          const base = typed.kind === "abs" ? typed.value : typed.kind === "rel" ? applyNumEdit(typed, value, { integer: false }).value : value;
          const delta = (e.key === "ArrowUp" ? 1 : -1) * unitOf(stepAt(base), e.shiftKey, e.altKey);
          const next = clamp(integer ? Math.round(base + delta) : Number.parseFloat((base + delta).toPrecision(12)));
          editing.current = true;
          setText(String(next));
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
