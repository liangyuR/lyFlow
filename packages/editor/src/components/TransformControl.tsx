// transform 参数的控件（param-recipe P2.6）：平移 xyz + 旋转 xyz（度），可切成 4×4 矩阵。
// 存储格式照 manifest 的约定（16 个数、行主序，lib/transform.ts）；带缩放或切变的矩阵拆不成 T·R，
// 只能在矩阵视图里改。参数面板与 Inspector 用的都是这一份（ParamControls 的映射表）。

import { useState } from "react";

import { asMatrix, clean, compose, decompose, IDENTITY, isRigid, summarize } from "../lib/transform";
import { valueEquals } from "../lib/params";
import type { ControlProps } from "./ParamControls";
import { NumberInput } from "./NumberInput";

const AXES = ["X", "Y", "Z"];

export function TransformControl({ param, value, disabled, onChange, nodeId }: ControlProps) {
  const m = asMatrix(value);
  const rigid = isRigid(m);
  const [view, setView] = useState<"tr" | "matrix">(rigid ? "tr" : "matrix");
  const shown = rigid ? view : "matrix";
  const tr = rigid ? decompose(m) : null;
  const step = param.step ?? 0.001;

  // 只改平移时旋转块原样不动：从角度再合成一遍会在第 12 位上带出舍入差，文件 diff 里平白多几个数
  const setT = (i: number, v: number) => {
    const next = m.slice();
    next[[3, 7, 11][i]!] = clean(v);
    onChange(next);
  };
  const setR = (i: number, v: number) => {
    if (!tr) return;
    const r = [...tr.r] as [number, number, number];
    r[i] = v;
    onChange(compose({ t: tr.t, r }));
  };
  const setCell = (i: number, v: number) => {
    const next = m.slice();
    next[i] = clean(v);
    onChange(next);
  };

  return (
    <div className="ctl-tf" data-testid={`transform-${param.name}`} data-view={shown}>
      <div className="ctl-tf__head">
        <code className="ctl-tf__summary" data-testid={`transform-summary-${param.name}`}>
          {summarize(m)}
        </code>
        <button
          type="button"
          className="ctl-btn"
          data-testid={`transform-view-${param.name}`}
          disabled={!rigid}
          title={rigid ? "在 平移/旋转 与 4×4 矩阵 之间切换" : "含缩放或切变，只能按矩阵改"}
          onClick={() => setView(shown === "tr" ? "matrix" : "tr")}
        >
          {shown === "tr" ? "4×4" : "T·R"}
        </button>
        <button
          type="button"
          className="ctl-btn"
          disabled={disabled || valueEquals(m, IDENTITY)}
          title="设为单位阵（不平移、不旋转）"
          onClick={() => onChange([...IDENTITY])}
        >
          单位阵
        </button>
      </div>
      {shown === "tr" && tr ? (
        <div className="ctl-tf__tr">
          <span className="ctl-tf__label">T{param.unit ? ` (${param.unit})` : ""}</span>
          {tr.t.map((v, i) => (
            <label key={`t${i}`} className="ctl-vec__item">
              <span className="ctl-vec__axis">{AXES[i]}</span>
              <NumberInput
                value={v}
                disabled={disabled}
                integer={false}
                step={step}
                dragStep={step}
                dragName={`${param.name}-t${i}`}
                nodeId={nodeId}
                onCommit={(n) => setT(i, n)}
              />
            </label>
          ))}
          <span className="ctl-tf__label" title="内旋 X→Y→Z（R = Rz·Ry·Rx），与 transform.make 同一约定">
            R (°)
          </span>
          {tr.r.map((v, i) => (
            <label key={`r${i}`} className="ctl-vec__item">
              <span className="ctl-vec__axis">{AXES[i]}</span>
              <NumberInput
                value={v}
                disabled={disabled}
                integer={false}
                step={1}
                dragStep={1}
                dragName={`${param.name}-r${i}`}
                nodeId={nodeId}
                onCommit={(n) => setR(i, n)}
              />
            </label>
          ))}
        </div>
      ) : (
        <div className="ctl-tf__matrix" title="行主序：第一行是 m00 m01 m02 tx">
          {m.map((v, i) => (
            <NumberInput
              key={i}
              value={v}
              disabled={disabled}
              integer={false}
              step={step}
              dragStep={i % 4 === 3 ? step : 0.01}
              dragName={`${param.name}-m${i}`}
              nodeId={nodeId}
              onCommit={(n) => setCell(i, n)}
            />
          ))}
        </div>
      )}
    </div>
  );
}
