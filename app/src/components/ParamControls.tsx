//
// 参数控件。
//
// **这里是前端唯一的「算子知识」**：一张 param.type -> React 控件的映射表
// （docs/operator-manifest.md）。新增一种参数类型的成本是这里加一个 case，
// 有界且可接受；新增一个算子的成本是零。
//
// 撤销粒度的处理贯穿全文件：
//   输入框  —— 本地 state，失焦/回车才提交，一次编辑一条撤销
//   滑块    —— 按下时 begin()，松开时 commit()，一次拖动一条撤销
// 少了这层，拖一次滑块会在撤销栈里塞几十条，Ctrl+Z 变成没有意义的操作。
//

import { useEffect, useRef, useState } from "react";

import { useGraphStore } from "../store/graph";
import { useUiStore } from "../store/ui";
import type { EnumOption, Param } from "../types/manifest";

export interface ControlProps {
  param: Param;
  value: unknown;
  disabled: boolean;
  onChange: (value: unknown) => void;
}

// ---------------------------------------------------------------- 数值输入

interface NumberInputProps {
  value: number;
  disabled: boolean;
  integer: boolean;
  min?: number | undefined;
  max?: number | undefined;
  step?: number | undefined;
  onCommit: (v: number) => void;
}

function NumberInput({ value, disabled, integer, min, max, step, onCommit }: NumberInputProps) {
  const [text, setText] = useState(String(value));
  const editing = useRef(false);

  // 外部值变了（撤销、切换节点）且用户没在编辑时才同步，
  // 否则会在用户打字的中途把输入框内容抢走。
  useEffect(() => {
    if (!editing.current) setText(String(value));
  }, [value]);

  const commit = () => {
    editing.current = false;
    const parsed = integer ? parseInt(text, 10) : parseFloat(text);
    if (Number.isNaN(parsed)) {
      setText(String(value)); // 输入非法，恢复原值而不是写入 NaN
      return;
    }
    let next = parsed;
    if (min !== undefined) next = Math.max(next, min);
    if (max !== undefined) next = Math.min(next, max);
    setText(String(next));
    if (next !== value) onCommit(next);
  };

  return (
    <input
      className="ctl ctl--num"
      type="number"
      disabled={disabled}
      value={text}
      step={step ?? (integer ? 1 : "any")}
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
  onDrag,
}: {
  value: number;
  min: number;
  max: number;
  step: number | undefined;
  disabled: boolean;
  onDrag: (v: number) => void;
}) {
  return (
    <input
      className="ctl ctl--slider"
      type="range"
      disabled={disabled}
      min={min}
      max={max}
      step={step ?? (max - min) / 200}
      value={Math.min(Math.max(value, min), max)}
      onPointerDown={() => useGraphStore.getState().begin()}
      onPointerUp={() => useGraphStore.getState().commit("拖动参数")}
      onChange={(e) => onDrag(parseFloat(e.target.value))}
    />
  );
}

function NumberControl({ param, value, disabled, onChange }: ControlProps) {
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
        onCommit={onChange}
      />
      {hasSlider && (
        <Slider
          value={num}
          min={sMin}
          max={sMax}
          step={param.step}
          disabled={disabled}
          onDrag={(v) => onChange(integer ? Math.round(v) : v)}
        />
      )}
      {param.unit && <span className="ctl-unit">{param.unit}</span>}
    </div>
  );
}

// ---------------------------------------------------------------- 向量

const DEFAULT_COMPONENTS = ["X", "Y", "Z", "W"];

function VectorControl({ param, value, disabled, onChange }: ControlProps) {
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

// ------------------------------------------------- param.type -> 控件 映射表

export function ParamControl(props: ControlProps) {
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
