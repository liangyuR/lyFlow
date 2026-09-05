// 参数控件。**前端唯一的「算子知识」**：一张 param.type -> React 控件的映射表（docs/operator-manifest.md）。
// 撤销粒度贯穿全文件：输入框失焦/回车才提交，滑块与数字框拖动用 begin()/commit() 包住一整段（见 README）。

import { useCallback, useEffect, useRef, useState } from "react";

import { beginPreview, endPreview, schedulePreview } from "../lib/preview";
import { useGraphStore } from "../store/graph";
import { useUiStore } from "../store/ui";
import type { EnumOption, Param } from "../types/manifest";

import "../styles.params.css";

export interface ControlProps {
  param: Param;
  value: unknown;
  disabled: boolean;
  onChange: (value: unknown) => void;
  /** 所在节点。live preview 要用它当 run 的目标，参数右键要用它做提升。 */
  nodeId?: string | undefined;
  /** 这个内参已经被提升成哪个子图参数了（F4）。 */
  promotedAs?: string | undefined;
}

// ---------------------------------------------------------------- 数值输入

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
function dragStepOf(param: Param, integer: boolean): number {
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

interface NumberInputProps {
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

function NumberInput({
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
function Slider({
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

function NumberControl({ param, value, disabled, onChange, nodeId }: ControlProps) {
  const integer = param.type === "int";
  const num = typeof value === "number" ? value : 0;
  const sMin = param.softMin ?? param.min;
  const sMax = param.softMax ?? param.max;
  const hasSlider = sMin !== undefined && sMax !== undefined && Number.isFinite(sMin) && Number.isFinite(sMax);

  return (
    <div className="ctl-row">
      <NumberInput
        value={num}
        disabled={disabled}
        integer={integer}
        min={param.min}
        max={param.max}
        step={param.step}
        dragStep={dragStepOf(param, integer)}
        dragName={param.name}
        nodeId={nodeId}
        onCommit={onChange}
      />
      {hasSlider && (
        <Slider
          value={num}
          min={sMin}
          max={sMax}
          step={param.step}
          disabled={disabled}
          nodeId={nodeId}
          testId={`param-slider-${param.name}`}
          onDrag={(v) => onChange(integer ? Math.round(v) : v)}
        />
      )}
      {param.unit && <span className="ctl-unit">{param.unit}</span>}
    </div>
  );
}

// ---------------------------------------------------------------- 向量

const DEFAULT_COMPONENTS = ["X", "Y", "Z", "W"];

function VectorControl({ param, value, disabled, onChange, nodeId }: ControlProps) {
  const size = param.type === "vec2f" ? 2 : param.type === "vec3f" ? 3 : 4;
  const vec = Array.isArray(value) ? (value as number[]) : new Array<number>(size).fill(0);
  const labels = param.componentLabels ?? DEFAULT_COMPONENTS;

  // 三个分量通常要保持一致（比如体素边长），锁上之后改一个分量全跟着走。
  const [locked, setLocked] = useState(false);

  const setComponent = (i: number, v: number) => {
    const next = locked ? new Array<number>(size).fill(v) : vec.slice();
    if (!locked) next[i] = v;
    onChange(next);
  };

  return (
    <div className="ctl-row ctl-row--vec">
      <div className="ctl-vec">
        {Array.from({ length: size }, (_, i) => (
          <label key={i} className="ctl-vec__item">
            <span className="ctl-vec__axis">{labels[i] ?? DEFAULT_COMPONENTS[i]}</span>
            <NumberInput
              value={vec[i] ?? 0}
              disabled={disabled}
              integer={false}
              min={param.min}
              max={param.max}
              step={param.step}
              dragStep={dragStepOf(param, false)}
              dragName={`${param.name}-${i}`}
              nodeId={nodeId}
              onCommit={(v) => setComponent(i, v)}
            />
          </label>
        ))}
      </div>
      <button
        type="button"
        className={`ctl-lock${locked ? " is-on" : ""}`}
        disabled={disabled}
        title={locked ? "已锁定：改一个分量，其余跟随" : "锁定分量联动"}
        onClick={() => setLocked((v) => !v)}
      >
        {locked ? "🔗" : "⛓"}
      </button>
      {param.unit && <span className="ctl-unit">{param.unit}</span>}
    </div>
  );
}

// ---------------------------------------------------------------- 其余类型

function BoolControl({ value, disabled, onChange }: ControlProps) {
  return (
    <label className="ctl-check">
      <input
        type="checkbox"
        disabled={disabled}
        checked={value === true}
        onChange={(e) => onChange(e.target.checked)}
      />
    </label>
  );
}

function EnumControl({ param, value, disabled, onChange }: ControlProps) {
  const options: EnumOption[] = param.options ?? [];
  const current = options.find((o) => o.value === value);
  return (
    <div className="ctl-row">
      <select
        className="ctl ctl--select"
        disabled={disabled}
        value={String(value ?? "")}
        onChange={(e) => {
          // manifest 里 enum 的 value 可能是数字，选择框只会给字符串，
          // 按原始选项的类型还原，否则写回 GraphDoc 的类型会漂移。
          const picked = options.find((o) => String(o.value) === e.target.value);
          onChange(picked ? picked.value : e.target.value);
        }}
      >
        {options.map((o) => (
          <option key={String(o.value)} value={String(o.value)}>
            {o.label}
          </option>
        ))}
      </select>
      {current?.doc && <span className="ctl-hint">{current.doc}</span>}
    </div>
  );
}

function FlagsControl({ param, value, disabled, onChange }: ControlProps) {
  const options: EnumOption[] = param.options ?? [];
  const bits = typeof value === "number" ? value : 0;
  return (
    <div className="ctl-flags">
      {options.map((o) => {
        const bit = typeof o.value === "number" ? o.value : 0;
        const on = (bits & bit) !== 0;
        return (
          <button
            key={String(o.value)}
            type="button"
            className={`ctl-chip${on ? " is-on" : ""}`}
            disabled={disabled}
            title={o.doc}
            onClick={() => onChange(on ? bits & ~bit : bits | bit)}
          >
            {o.label}
          </button>
        );
      })}
    </div>
  );
}

function TextishControl({ param, value, disabled, onChange }: ControlProps) {
  const [text, setText] = useState(String(value ?? ""));
  const editing = useRef(false);
  useEffect(() => {
    if (!editing.current) setText(String(value ?? ""));
  }, [value]);

  const commit = () => {
    editing.current = false;
    if (text !== value) onChange(text);
  };

  const shared = {
    disabled,
    value: text,
    onFocus: () => (editing.current = true),
    onChange: (e: React.ChangeEvent<HTMLInputElement | HTMLTextAreaElement>) => {
      editing.current = true;
      setText(e.target.value);
    },
    onBlur: commit,
  };

  if (param.type === "text") {
    return <textarea className="ctl ctl--text" rows={param.rows ?? 3} {...shared} />;
  }
  return (
    <input
      className="ctl ctl--str"
      type="text"
      placeholder={param.placeholder}
      {...shared}
      onKeyDown={(e) => {
        if (e.key === "Enter") e.currentTarget.blur();
        if (e.key === "Escape") {
          editing.current = false;
          setText(String(value ?? ""));
          e.currentTarget.blur();
        }
      }}
    />
  );
}

function PathControl({ param, value, disabled, onChange }: ControlProps) {
  const pick = async () => {
    if (!("__TAURI_INTERNALS__" in window)) {
      useUiStore.getState().showToast("浏览器模式没有文件对话框，请在 Tauri 里运行", "warn");
      return;
    }
    const { open, save } = await import("@tauri-apps/plugin-dialog");
    const filters = (param.filters ?? []).map((f) => ({ name: f.name, extensions: f.extensions }));
    const picked =
      param.mode === "save"
        ? await save({ filters })
        : await open({ directory: param.mode === "dir", multiple: false, filters });
    if (typeof picked === "string") onChange(picked);
  };

  return (
    <div className="ctl-row">
      <TextishControl
        param={{ ...param, type: "string" }}
        value={value}
        disabled={disabled}
        onChange={onChange}
      />
      <button type="button" className="ctl-btn" disabled={disabled} onClick={() => void pick()}>
        浏览…
      </button>
    </div>
  );
}

function toHex(v: unknown): string {
  if (!Array.isArray(v)) return "#000000";
  const [r = 0, g = 0, b = 0] = v as number[];
  const c = (x: number) =>
    Math.round(Math.min(Math.max(x, 0), 1) * 255)
      .toString(16)
      .padStart(2, "0");
  return `#${c(r)}${c(g)}${c(b)}`;
}

function ColorControl({ value, disabled, onChange }: ControlProps) {
  const arr = Array.isArray(value) ? (value as number[]) : [0, 0, 0];
  return (
    <input
      className="ctl ctl--color"
      type="color"
      disabled={disabled}
      value={toHex(value)}
      onChange={(e) => {
        const hex = e.target.value;
        const rgb = [1, 3, 5].map((i) => parseInt(hex.slice(i, i + 2), 16) / 255);
        // 保留原有 alpha 分量，取色器只给 RGB
        onChange(arr.length === 4 ? [...rgb, arr[3] ?? 1] : rgb);
      }}
    />
  );
}

function UnsupportedControl({ param }: ControlProps) {
  return (
    <span className="ctl-unsupported">
      <code>{param.type}</code> 控件尚未实现
    </span>
  );
}

// ----------------------------------------------------------- 参数右键（#26）

type Coerced = { ok: true; value: unknown } | { ok: false; msg: string };

/** 数组类型能接受的长度。color 的 alpha 可有可无，transform 以默认值的长度为准。 */
function arrayLengths(param: Param): number[] {
  switch (param.type) {
    case "vec2f":
      return [2];
    case "vec3f":
      return [3];
    case "vec4f":
      return [4];
    case "color":
      return [3, 4];
    case "transform":
      return [Array.isArray(param.default) ? param.default.length : 16];
    default:
      return [];
  }
}

function clampToParam(param: Param, v: number): number {
  let n = v;
  if (param.min !== undefined) n = Math.max(n, param.min);
  if (param.max !== undefined) n = Math.min(n, param.max);
  return n;
}

/** 粘贴的形状校验。对不上就退回一条人话，绝不往 GraphDoc 里塞坏值。 */
function coerceValue(param: Param, raw: unknown): Coerced {
  const isNum = typeof raw === "number" && Number.isFinite(raw);
  switch (param.type) {
    case "bool":
      return typeof raw === "boolean"
        ? { ok: true, value: raw }
        : { ok: false, msg: `粘贴失败：${param.name} 需要 true / false` };
    case "int":
    case "flags":
      return isNum
        ? { ok: true, value: clampToParam(param, Math.round(raw)) }
        : { ok: false, msg: `粘贴失败：${param.name} 需要一个整数` };
    case "float":
      return isNum
        ? { ok: true, value: clampToParam(param, raw) }
        : { ok: false, msg: `粘贴失败：${param.name} 需要一个数字` };
    case "enum": {
      const hit = (param.options ?? []).some((o) => o.value === raw);
      return hit
        ? { ok: true, value: raw }
        : { ok: false, msg: `粘贴失败：${JSON.stringify(raw)} 不是 ${param.name} 的合法选项` };
    }
    case "string":
    case "text":
    case "path":
      return typeof raw === "string"
        ? { ok: true, value: raw }
        : { ok: false, msg: `粘贴失败：${param.name} 需要一个字符串` };
    case "vec2f":
    case "vec3f":
    case "vec4f":
    case "color":
    case "transform": {
      const want = arrayLengths(param);
      const ok =
        Array.isArray(raw) &&
        want.includes(raw.length) &&
        raw.every((v) => typeof v === "number" && Number.isFinite(v));
      return ok
        ? { ok: true, value: raw }
        : { ok: false, msg: `粘贴失败：${param.name} 需要长度 ${want.join(" 或 ")} 的数字数组` };
    }
    case "curve":
      return { ok: false, msg: `粘贴失败：${param.name} 是 curve，暂不支持` };
  }
}

/** 剪贴板可能因为不安全上下文或没授权而不可用，一律吞掉异常返回失败。 */
async function copyText(text: string): Promise<boolean> {
  try {
    await navigator.clipboard.writeText(text);
    return true;
  } catch {
    return legacyCopy(text);
  }
}

function legacyCopy(text: string): boolean {
  try {
    const ta = document.createElement("textarea");
    ta.value = text;
    ta.style.cssText = "position:fixed;top:-1000px;opacity:0";
    document.body.appendChild(ta);
    ta.select();
    const ok = document.execCommand("copy");
    ta.remove();
    return ok;
  } catch {
    return false;
  }
}

async function readClipboard(): Promise<string | null> {
  try {
    return await navigator.clipboard.readText();
  } catch {
    return null;
  }
}

function ParamMenu({
  param,
  value,
  x,
  y,
  nodeId,
  promotedAs,
  onChange,
  onClose,
}: {
  param: Param;
  value: unknown;
  x: number;
  y: number;
  nodeId?: string | undefined;
  promotedAs?: string | undefined;
  onChange: (v: unknown) => void;
  onClose: () => void;
}) {
  const inSubgraph = useUiStore((s) => s.path.length > 0);
  const ref = useRef<HTMLDivElement>(null);

  useEffect(() => {
    const onDown = (e: MouseEvent) => {
      if (!ref.current?.contains(e.target as Node)) onClose();
    };
    const onKey = (e: KeyboardEvent) => {
      if (e.key !== "Escape") return;
      e.stopPropagation(); // Esc 先关菜单，不要顺带触发全局快捷键
      onClose();
    };
    document.addEventListener("mousedown", onDown, true);
    document.addEventListener("keydown", onKey, true);
    return () => {
      document.removeEventListener("mousedown", onDown, true);
      document.removeEventListener("keydown", onKey, true);
    };
  }, [onClose]);

  const copy = (text: string, what: string) => {
    onClose();
    void copyText(text).then((ok) => {
      const msg = ok ? `已复制${what}` : "剪贴板不可用，复制失败";
      useUiStore.getState().showToast(msg, ok ? "info" : "warn");
    });
  };

  const paste = () => {
    onClose();
    void readClipboard().then((text) => {
      const ui = useUiStore.getState();
      if (text === null) {
        ui.showToast("剪贴板不可用，粘贴失败", "warn");
        return;
      }
      let raw: unknown;
      try {
        raw = JSON.parse(text) as unknown;
      } catch {
        ui.showToast("粘贴失败：剪贴板内容不是合法 JSON", "warn");
        return;
      }
      const r = coerceValue(param, raw);
      if (!r.ok) {
        ui.showToast(r.msg, "warn");
        return;
      }
      onChange(r.value);
    });
  };

  return (
    <div
      ref={ref}
      className="ctxmenu param-menu"
      data-testid="param-menu"
      style={{ left: Math.max(4, Math.min(x, window.innerWidth - 180)), top: y }}
      onContextMenu={(e) => e.preventDefault()}
    >
      <button
        type="button"
        data-testid="param-menu-reset"
        title={`默认值 ${JSON.stringify(param.default)}`}
        onClick={() => {
          onChange(param.default);
          onClose();
        }}
      >
        重置为默认
      </button>
      <button
        type="button"
        data-testid="param-menu-copy"
        onClick={() => copy(JSON.stringify(value ?? null), `${param.name} 的值`)}
      >
        复制值
      </button>
      <button type="button" data-testid="param-menu-paste" onClick={paste}>
        粘贴值
      </button>
      <button
        type="button"
        data-testid="param-menu-path"
        title="给 CLI 的 --set <name>=<value> 用"
        onClick={() => copy(param.name, `参数名 ${param.name}`)}
      >
        复制路径名
      </button>
      {inSubgraph && nodeId && !promotedAs && (
        <button
          type="button"
          data-testid="param-menu-promote"
          title="提升成子图的对外参数，外层表单上就能改（F4）"
          onClick={() => {
            const name = useGraphStore.getState().promoteParam(nodeId, param.name);
            onClose();
            if (name) useUiStore.getState().showToast(`已提升为子图参数 ${name}`);
          }}
        >
          提升为子图参数
        </button>
      )}
      {inSubgraph && promotedAs && (
        <button
          type="button"
          data-testid="param-menu-unpromote"
          onClick={() => {
            useGraphStore.getState().unpromoteParam(promotedAs);
            onClose();
          }}
        >
          取消提升 {promotedAs}
        </button>
      )}
    </div>
  );
}

// ------------------------------------------------- param.type -> 控件 映射表

function ParamWidget(props: ControlProps) {
  switch (props.param.type) {
    case "bool":
      return <BoolControl {...props} />;
    case "int":
    case "float":
      return <NumberControl {...props} />;
    case "vec2f":
    case "vec3f":
    case "vec4f":
      return <VectorControl {...props} />;
    case "enum":
      return <EnumControl {...props} />;
    case "flags":
      return <FlagsControl {...props} />;
    case "string":
    case "text":
      return <TextishControl {...props} />;
    case "path":
      return <PathControl {...props} />;
    case "color":
      return <ColorControl {...props} />;
    // transform / curve 需要专门的编辑器，是 P2。
    // 显式列出来而不是落到 default，这样 manifest 新增类型时 TS 会报错提醒。
    case "transform":
    case "curve":
      return <UnsupportedControl {...props} />;
  }
}

export function ParamControl(props: ControlProps) {
  const wrap = useRef<HTMLDivElement>(null);
  const [menu, setMenu] = useState<{ x: number; y: number } | null>(null);

  // 右键挂到整行（.insp-param）上而不是控件上：标签那一片也要能唤出菜单，
  // 而那一行由 Inspector 渲染，这里只能顺着 DOM 往上找。
  useEffect(() => {
    const host = wrap.current?.closest(".insp-param") ?? wrap.current;
    if (!host) return;
    const onCtx = (e: Event) => {
      e.preventDefault();
      const me = e as MouseEvent;
      setMenu({ x: me.clientX, y: me.clientY });
    };
    host.addEventListener("contextmenu", onCtx);
    return () => host.removeEventListener("contextmenu", onCtx);
  }, []);

  const close = useCallback(() => setMenu(null), []);

  return (
    <div className="ctl-wrap" ref={wrap}>
      <ParamWidget {...props} />
      {menu && (
        <ParamMenu
          param={props.param}
          value={props.value}
          x={menu.x}
          y={menu.y}
          nodeId={props.nodeId}
          promotedAs={props.promotedAs}
          onChange={props.onChange}
          onClose={close}
        />
      )}
    </div>
  );
}
