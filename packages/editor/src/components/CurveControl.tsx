// curve 参数的控件（param-recipe P2.6）：一块小画布拖控制点 + 控制点列表。值格式见 lib/curve.ts
// （与 core 的 checkCurveValue / evaluateCurve 同一套）。撤销粒度与滑块相同：拖一个点从按下到松手一条撤销，
// 列表里改数、加点、删点、换插值方式各一条。参数面板与 Inspector 用的都是这一份。

import { useEffect, useRef, useState } from "react";

import {
  asCurve,
  evaluate,
  insertAt,
  insertInWidestGap,
  movePoint,
  normalized,
  removePoint,
  yRange,
  type CurveInterp,
  type CurvePoint,
} from "../lib/curve";
import { beginPreview, endPreview, schedulePreview } from "../lib/preview";
import { useGraphStore } from "../store/graph";
import type { ControlProps } from "./ParamControls";
import { NumberInput } from "./NumberInput";

const HEIGHT = 120;
const PAD = 8;
const SAMPLES = 64;

export function CurveControl({ param, value, disabled, onChange, nodeId }: ControlProps) {
  const curve = asCurve(value, param.default);
  const [lo, hi] = yRange(curve, param);
  const [selected, setSelected] = useState<number | null>(null);
  const [width, setWidth] = useState(240);
  const svgRef = useRef<SVGSVGElement>(null);
  const drag = useRef<{ id: number; index: number; range: [number, number] } | null>(null);

  // 画布跟着行宽走：按真实像素画，圆点才是圆的（preserveAspectRatio=none 会把它们压成椭圆）
  useEffect(() => {
    const el = svgRef.current;
    if (!el) return;
    const ro = new ResizeObserver(() => setWidth(Math.max(120, Math.round(el.clientWidth))));
    ro.observe(el);
    return () => ro.disconnect();
  }, []);

  const interp: CurveInterp | undefined = curve.interp;
  const write = (points: CurvePoint[], nextInterp: CurveInterp | undefined = interp) =>
    onChange(normalized(points, nextInterp));

  const range = drag.current?.range ?? [lo, hi];
  const sx = (x: number) => PAD + x * (width - 2 * PAD);
  const sy = (y: number) => PAD + (1 - (y - range[0]) / (range[1] - range[0])) * (HEIGHT - 2 * PAD);
  const toData = (clientX: number, clientY: number): [number, number] => {
    const r = svgRef.current!.getBoundingClientRect();
    const x = (clientX - r.left - PAD) / Math.max(1, r.width - 2 * PAD);
    const y = range[0] + (1 - (clientY - r.top - PAD) / Math.max(1, r.height - 2 * PAD)) * (range[1] - range[0]);
    return [x, y];
  };

  const path = Array.from({ length: SAMPLES + 1 }, (_, i) => {
    const x = i / SAMPLES;
    return `${i === 0 ? "M" : "L"}${sx(x).toFixed(1)},${sy(evaluate(curve, x)).toFixed(1)}`;
  }).join(" ");

  const onPointDown = (e: React.PointerEvent<SVGCircleElement>, index: number) => {
    if (disabled || e.button !== 0) return;
    e.preventDefault();
    e.stopPropagation();
    e.currentTarget.setPointerCapture(e.pointerId);
    // 拖动期间 y 轴不跟着缩放：点被拖出上沿时范围一撑开，整条线会在手底下跳
    drag.current = { id: e.pointerId, index, range: [lo, hi] };
    setSelected(index);
    useGraphStore.getState().begin();
    if (nodeId) beginPreview(nodeId);
  };
  const onPointMove = (e: React.PointerEvent<SVGCircleElement>) => {
    const d = drag.current;
    if (!d || d.id !== e.pointerId) return;
    const [x, y] = toData(e.clientX, e.clientY);
    write(movePoint(curve.points, d.index, x, y, param.min, param.max));
    if (nodeId) schedulePreview(nodeId);
  };
  const onPointUp = (e: React.PointerEvent<SVGCircleElement>) => {
    const d = drag.current;
    if (!d || d.id !== e.pointerId) return;
    drag.current = null;
    if (e.currentTarget.hasPointerCapture(e.pointerId)) e.currentTarget.releasePointerCapture(e.pointerId);
    useGraphStore.getState().commit("拖动曲线控制点");
    endPreview(nodeId);
  };

  const addAt = (clientX: number, clientY: number) => {
    const [x] = toData(clientX, clientY);
    const hit = insertAt(curve, x);
    if (!hit) return;
    setSelected(hit.index);
    write(hit.points);
  };

  return (
    <div className="ctl-curve" data-testid={`curve-${param.name}`} data-points={curve.points.length}>
      <svg
        ref={svgRef}
        className={`ctl-curve__canvas${disabled ? " is-disabled" : ""}`}
        data-testid={`curve-canvas-${param.name}`}
        data-range={`${range[0]},${range[1]}`}
        data-pad={PAD}
        height={HEIGHT}
        viewBox={`0 0 ${width} ${HEIGHT}`}
        onDoubleClick={(e) => {
          if (!disabled) addAt(e.clientX, e.clientY);
        }}
      >
        <rect className="ctl-curve__frame" x={PAD} y={PAD} width={width - 2 * PAD} height={HEIGHT - 2 * PAD} />
        {[0.25, 0.5, 0.75].map((g) => (
          <line key={`gx${g}`} className="ctl-curve__grid" x1={sx(g)} x2={sx(g)} y1={PAD} y2={HEIGHT - PAD} />
        ))}
        {[0.25, 0.5, 0.75].map((g) => (
          <line
            key={`gy${g}`}
            className="ctl-curve__grid"
            x1={PAD}
            x2={width - PAD}
            y1={PAD + g * (HEIGHT - 2 * PAD)}
            y2={PAD + g * (HEIGHT - 2 * PAD)}
          />
        ))}
        <path className="ctl-curve__line" d={path} />
        {curve.points.map(([x, y], i) => (
          <circle
            key={i}
            className={`ctl-curve__pt${selected === i ? " is-selected" : ""}`}
            data-testid={`curve-pt-${param.name}-${i}`}
            data-x={x}
            data-y={y}
            cx={sx(x)}
            cy={sy(y)}
            r={5}
            onPointerDown={(e) => onPointDown(e, i)}
            onPointerMove={onPointMove}
            onPointerUp={onPointUp}
            onPointerCancel={onPointUp}
            onDoubleClick={(e) => {
              e.stopPropagation();
              if (disabled) return;
              const next = removePoint(curve.points, i);
              if (next) {
                setSelected(null);
                write(next);
              }
            }}
          >
            <title>{`(${x}, ${y}) —— 拖动改位置，双击删除`}</title>
          </circle>
        ))}
      </svg>
      <div className="ctl-curve__bar">
        <select
          className="ctl ctl--select ctl-curve__interp"
          data-testid={`curve-interp-${param.name}`}
          disabled={disabled}
          value={interp ?? "linear"}
          onChange={(e) => write(curve.points, e.target.value as CurveInterp)}
        >
          <option value="linear">线性</option>
          <option value="smooth">平滑（单调三次）</option>
        </select>
        <button
          type="button"
          className="ctl-btn"
          data-testid={`curve-add-${param.name}`}
          disabled={disabled}
          title="在最宽的一段中间加一个控制点；也可以在曲线区域里双击"
          onClick={() => {
            const hit = insertInWidestGap(curve);
            if (!hit) return;
            setSelected(hit.index);
            write(hit.points);
          }}
        >
          + 控制点
        </button>
      </div>
      <div className="ctl-curve__list" data-testid={`curve-list-${param.name}`}>
        <div className="ctl-curve__row ctl-curve__row--head" aria-hidden>
          <span />
          <span>x（0–1）</span>
          <span>y{param.unit ? `（${param.unit}）` : ""}</span>
          <span />
        </div>
        {curve.points.map(([x, y], i) => (
          <div
            key={i}
            className={`ctl-curve__row${selected === i ? " is-selected" : ""}`}
            data-testid={`curve-row-${param.name}-${i}`}
            onFocus={() => setSelected(i)}
          >
            <span className="ctl-vec__axis">{i + 1}</span>
            <NumberInput
              value={x}
              disabled={disabled}
              integer={false}
              min={0}
              max={1}
              step={0.01}
              dragStep={0.01}
              nodeId={nodeId}
              onCommit={(n) => write(movePoint(curve.points, i, n, y, param.min, param.max))}
            />
            <NumberInput
              value={y}
              disabled={disabled}
              integer={false}
              min={param.min}
              max={param.max}
              step={param.step}
              dragStep={param.step ?? (hi - lo) / 100}
              nodeId={nodeId}
              onCommit={(n) => write(movePoint(curve.points, i, x, n, param.min, param.max))}
            />
            <button
              type="button"
              className="ctl-btn"
              disabled={disabled || curve.points.length <= 2}
              title="删掉这个控制点（至少留两个）"
              onClick={() => {
                const next = removePoint(curve.points, i);
                if (next) write(next);
              }}
            >
              ✕
            </button>
          </div>
        ))}
      </div>
    </div>
  );
}
