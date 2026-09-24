// 参数控件。**前端唯一的「算子知识」**：一张 param.type -> React 控件的映射表（docs/operator-manifest.md）。
// 撤销粒度贯穿全文件：输入框失焦/回车才提交，滑块与数字框拖动用 begin()/commit() 包住一整段（见 README）。

import { useCallback, useEffect, useRef, useState } from "react";

import { curveProblem } from "../lib/curve";
import { dialogs } from "../lib/dialogs";
import { joinBind, type GraphBinding } from "../lib/graphParams";
import { fullId, type SubPath } from "../lib/subgraph";
import { useGraphStore } from "../store/graph";
import { useUiStore } from "../store/ui";
import type { EnumOption, Param } from "../types/manifest";

import { CurveControl } from "./CurveControl";
import { dragStepOf, NumberInput, Slider } from "./NumberInput";
import { TransformControl } from "./TransformControl";

import "../styles.params.css";

export interface ControlProps {
  param: Param;
  value: unknown;
  disabled: boolean;
  onChange: (value: unknown) => void;
  /** 所在节点。live preview 要用它当 run 的目标，参数右键要用它做提升。 */
  nodeId?: string | undefined;
  /** 节点所在的层级。不给 = 当前层级（ui.path）；参数面板展开进子图定义的那几行给的是更深一层
   *  （param-recipe P2.3）。live preview 的目标、右键里的提升 / 复制路径都按它算。 */
  path?: SubPath | undefined;
  /** 这个内参已经被提升成哪个子图参数了（F4）。 */
  promotedAs?: string | undefined;
  /** 这个参数最终由哪个顶层图参数提供（param-recipe P1.4）。右键菜单据此给「纳入配方」或「解除绑定」。 */
  graphBinding?: GraphBinding | null | undefined;
}

// ---------------------------------------------------------------- 数值

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
        // 老的 manifest 把位写成了字符串（"4"），按数认，否则按位与永远是 0
        const bit = typeof o.value === "number" ? o.value : Number(o.value) || 0;
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
    const pickPath = dialogs().pickPath;
    if (!pickPath) {
      useUiStore.getState().showToast("当前宿主没有文件对话框，请手动填路径", "warn");
      return;
    }
    const filters = (param.filters ?? []).map((f) => ({ name: f.name, extensions: f.extensions }));
    const picked = await pickPath({
      mode: param.mode === "save" ? "save" : param.mode === "dir" ? "dir" : "open",
      filters,
    });
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

function ColorControl({ param, value, disabled, onChange, nodeId }: ControlProps) {
  const arr = Array.isArray(value) ? (value as number[]) : [0, 0, 0];
  const hex = toHex(value);
  const alpha = arr.length === 4 ? (arr[3] ?? 1) : null;
  return (
    <div className="ctl-row ctl-color">
      {/* 色块本身就是取色器；旁边写出十六进制，面板里扫一眼就对得上数 */}
      <input
        className="ctl ctl--color"
        type="color"
        disabled={disabled}
        value={hex}
        data-testid={`color-${param.name}`}
        onChange={(e) => {
          const next = e.target.value;
          const rgb = [1, 3, 5].map((i) => parseInt(next.slice(i, i + 2), 16) / 255);
          // 保留原有 alpha 分量，取色器只给 RGB
          onChange(alpha !== null ? [...rgb, alpha] : rgb);
        }}
      />
      <code className="ctl-color__hex">{hex}</code>
      {alpha !== null && (
        <label className="ctl-vec__item" title="不透明度 0–1">
          <span className="ctl-vec__axis">A</span>
          <NumberInput
            value={alpha}
            disabled={disabled}
            integer={false}
            min={0}
            max={1}
            step={0.01}
            dragStep={0.01}
            dragName={`${param.name}-a`}
            nodeId={nodeId}
            onCommit={(a) => onChange([arr[0] ?? 0, arr[1] ?? 0, arr[2] ?? 0, a])}
          />
        </label>
      )}
    </div>
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
      return [16];
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
    case "curve": {
      const problem = curveProblem(raw, param.min, param.max);
      return problem === null
        ? { ok: true, value: raw }
        : { ok: false, msg: `粘贴失败：${param.name} 的曲线不合法 —— ${problem}` };
    }
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
  at,
  promotedAs,
  graphBinding,
  onChange,
  onClose,
}: {
  param: Param;
  value: unknown;
  x: number;
  y: number;
  nodeId?: string | undefined;
  at?: SubPath | undefined;
  promotedAs?: string | undefined;
  graphBinding?: GraphBinding | null | undefined;
  onChange: (v: unknown) => void;
  onClose: () => void;
}) {
  const uiPath = useUiStore((s) => s.path);
  const path = at ?? uiPath;
  const inSubgraph = path.length > 0;
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
        title="节点.参数 —— 顶层节点的这一串就是 CLI 的 --set <节点>.<参数>=<json>；子图里是展开后的路径"
        onClick={() => {
          // P1.6：带上节点 id。子图里给展开后的路径 id（与 lyflow params、事件里的 nodeId 同一套）
          const text = nodeId ? `${fullId(path, nodeId)}.${param.name}` : param.name;
          copy(text, ` ${text}`);
        }}
      >
        复制路径名
      </button>
      {nodeId && !graphBinding && (
        <button
          type="button"
          data-testid="param-menu-include"
          title={
            inSubgraph
              ? "逐层提升：内参 → 子图参数 → 这个实例上绑成图参数；同一子图的其它实例行为不变"
              : "提升为顶层图参数：当前值成为它的默认值，这一行此后改的是图参数"
          }
          onClick={() => {
            const name = useGraphStore.getState().promoteToGraphParam(nodeId, param.name, at);
            onClose();
            if (name) useUiStore.getState().showToast(`已纳入配方：图参数 ${name}`);
          }}
        >
          纳入配方（提升为图参数）
        </button>
      )}
      {graphBinding && (
        <button
          type="button"
          data-testid="param-menu-unbind"
          title="把图参数当前的值写回这个参数，行为不变"
          onClick={() => {
            useGraphStore
              .getState()
              .unbindFromGraphParam(
                graphBinding.graphParam,
                joinBind(graphBinding.top.node, graphBinding.top.param),
              );
            onClose();
          }}
        >
          解除图参数 {graphBinding.graphParam} 的绑定
        </button>
      )}
      {inSubgraph && nodeId && !promotedAs && (
        <button
          type="button"
          data-testid="param-menu-promote"
          title="提升成子图的对外参数，外层表单上就能改（F4）"
          onClick={() => {
            const name = useGraphStore.getState().promoteParam(nodeId, param.name, at);
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
            useGraphStore.getState().unpromoteParam(promotedAs, at);
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
    // 显式列全 14 种而不是落到 default，这样 manifest 新增类型时 TS 会报错提醒。
    case "transform":
      return <TransformControl {...props} />;
    case "curve":
      return <CurveControl {...props} />;
  }
}

export function ParamControl(props: ControlProps) {
  const wrap = useRef<HTMLDivElement>(null);
  const [menu, setMenu] = useState<{ x: number; y: number } | null>(null);
  const uiPath = useUiStore((s) => s.path);
  // live preview 的目标是「相对当前层级」的 id：fire() 会再拼上 ui.path 的前缀。
  // 参数面板展开进子图定义的那几行在更深一层，这里把中间那几段实例补上
  const previewId =
    props.nodeId && props.path && props.path.length > uiPath.length
      ? [...props.path.slice(uiPath.length).map((seg) => seg.nodeId), props.nodeId].join("/")
      : props.nodeId;

  // 右键挂到整行（Inspector 的 .insp-param、参数面板的 .prow）上而不是控件上：标签那一片也要能
  // 唤出菜单，而那一行由外面渲染，这里只能顺着 DOM 往上找。
  useEffect(() => {
    const host = wrap.current?.closest(".insp-param, .prow") ?? wrap.current;
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
      <ParamWidget {...props} nodeId={previewId} />
      {menu && (
        <ParamMenu
          param={props.param}
          value={props.value}
          x={menu.x}
          y={menu.y}
          nodeId={props.nodeId}
          at={props.path}
          promotedAs={props.promotedAs}
          graphBinding={props.graphBinding}
          onChange={props.onChange}
          onClose={close}
        />
      )}
    </div>
  );
}
