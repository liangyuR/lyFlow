import { useEffect, useMemo, useRef, useState } from "react";
import * as THREE from "three";

import { num } from "../Inspector";
import { registerPeekCanvas } from "../../lib/peekCanvas";
import { RAMPS, type RampName } from "../../lib/ramps";
import {
  PEEK_FROZEN,
  usePeekStore,
  type PeekOpts,
  type TensorLayout,
} from "../../store/peek";
import { transport } from "../../transport";
import { decodeTensor, type TensorPayload } from "../../types/execution";
import type { PeekViewProps } from "./types";

import "../../styles.peek.tensor.css";

const MAX_SLICE_ELEMENTS = 4_194_304;
const MAX_EDGE = 8192;
const RAMP_NAMES: RampName[] = ["viridis", "gray", "jet"];
const LAYOUT_NAMES: TensorLayout[] = ["auto", "HWC", "CHW", "NHWC", "NCHW"];
const LINE_COLOR = "#4a9eff";
const ZERO_COLOR = "#454d5c";
const BAD_COLOR = "#ff00ff";

type TensorKind = "image" | "line" | "none";

interface SlicePlan {
  kind: TensorKind;
  layout: string;
  rank: number;
  w: number;
  h: number;
  sliceCount: number;
  channelCount: number;
  slice: number;
  rgb: boolean;
  channel: number;
  offset: number;
  count: number;
  blockChannels: number;
  interleaved: boolean;
  picks: number[];
}

function dim(shape: readonly number[], i: number): number {
  const v = shape[i];
  if (typeof v !== "number" || !Number.isFinite(v) || v < 0) return 0;
  return Math.floor(v);
}

function isChannelish(v: number): boolean {
  return v === 1 || v === 3 || v === 4;
}

function inferLayout(shape: readonly number[]): string {
  const rank = shape.length;
  if (rank === 1) return "1D";
  if (rank === 2) return "HW";
  if (rank === 3) {
    if (isChannelish(dim(shape, 2))) return "HWC";
    if (isChannelish(dim(shape, 0))) return "CHW";
    return "NHW";
  }
  if (rank === 4) return isChannelish(dim(shape, 3)) ? "NHWC" : "NCHW";
  return "none";
}

function effectiveLayout(shape: readonly number[], choice: TensorLayout): string {
  const inferred = inferLayout(shape);
  if (choice === "auto") return inferred;
  const wanted = choice === "HWC" || choice === "CHW" ? 3 : 4;
  return shape.length === wanted ? choice : inferred;
}

function emptyPlan(rank: number, layout: string): SlicePlan {
  return {
    kind: "none",
    layout,
    rank,
    w: 0,
    h: 0,
    sliceCount: 0,
    channelCount: 0,
    slice: 0,
    rgb: false,
    channel: 0,
    offset: 0,
    count: 0,
    blockChannels: 0,
    interleaved: false,
    picks: [],
  };
}

function makePlan(
  shape: readonly number[] | null,
  choice: TensorLayout,
  sliceIndex: number,
  channelOpt: number | "rgb",
): SlicePlan {
  if (!shape || shape.length === 0) return emptyPlan(shape?.length ?? 0, "none");
  const layout = effectiveLayout(shape, choice);
  const rank = shape.length;
  if (layout === "none") return emptyPlan(rank, "none");

  if (layout === "1D") {
    const len = dim(shape, 0);
    return {
      kind: "line",
      layout,
      rank,
      w: len,
      h: 1,
      sliceCount: 0,
      channelCount: 1,
      slice: 0,
      rgb: false,
      channel: 0,
      offset: 0,
      count: len,
      blockChannels: 1,
      interleaved: false,
      picks: [0],
    };
  }

  let n = 0;
  let c = 1;
  let h = 1;
  let w = 1;
  let interleaved = false;
  if (layout === "HW") {
    h = dim(shape, 0);
    w = dim(shape, 1);
  } else if (layout === "HWC") {
    h = dim(shape, 0);
    w = dim(shape, 1);
    c = dim(shape, 2);
    interleaved = true;
  } else if (layout === "CHW") {
    c = dim(shape, 0);
    h = dim(shape, 1);
    w = dim(shape, 2);
  } else if (layout === "NHW") {
    n = dim(shape, 0);
    h = dim(shape, 1);
    w = dim(shape, 2);
  } else if (layout === "NHWC") {
    n = dim(shape, 0);
    h = dim(shape, 1);
    w = dim(shape, 2);
    c = dim(shape, 3);
    interleaved = true;
  } else {
    n = dim(shape, 0);
    c = dim(shape, 1);
    h = dim(shape, 2);
    w = dim(shape, 3);
  }

  const plane = w * h;
  const slice = n > 0 ? Math.min(Math.max(0, Math.floor(sliceIndex)), n - 1) : 0;
  const rgb = channelOpt === "rgb" && c >= 3;
  const asked = typeof channelOpt === "number" ? Math.floor(channelOpt) : 0;
  const channel = rgb ? 0 : Math.min(Math.max(0, asked), Math.max(0, c - 1));

  let offset = slice * plane;
  let count = plane;
  let blockChannels = 1;
  let picks = [0];
  if (interleaved) {
    offset = slice * plane * c;
    count = plane * c;
    blockChannels = c;
    picks = rgb ? [0, 1, 2] : [channel];
  } else if (c > 1) {
    if (rgb) {
      offset = slice * plane * c;
      count = plane * 3;
      blockChannels = 3;
      picks = [0, 1, 2];
    } else {
      offset = (slice * c + channel) * plane;
    }
  }

  return {
    kind: "image",
    layout,
    rank,
    w,
    h,
    sliceCount: n,
    channelCount: c,
    slice,
    rgb,
    channel,
    offset,
    count,
    blockChannels,
    interleaved,
    picks,
  };
}

function valueAt(data: Float32Array, plan: SlicePlan, pixel: number, j: number): number {
  const i = plan.interleaved ? pixel * plan.blockChannels + j : j * plan.w * plan.h + pixel;
  return data[i] ?? NaN;
}

function sliceExtent(payload: TensorPayload, plan: SlicePlan): { lo: number; hi: number } | null {
  const data = payload.data;
  let lo = Infinity;
  let hi = -Infinity;
  if (plan.kind === "line") {
    for (let i = 0; i < data.length; i += 1) {
      const v = data[i] ?? NaN;
      if (!Number.isFinite(v)) continue;
      if (v < lo) lo = v;
      if (v > hi) hi = v;
    }
  } else {
    const pixels = plan.w * plan.h;
    for (let p = 0; p < pixels; p += 1) {
      for (const j of plan.picks) {
        const v = valueAt(data, plan, p, j);
        if (!Number.isFinite(v)) continue;
        if (v < lo) lo = v;
        if (v > hi) hi = v;
      }
    }
  }
  return lo === Infinity ? null : { lo, hi };
}

function normalizer(lo: number, hi: number): (v: number) => number {
  const span = hi - lo;
  if (!Number.isFinite(span) || span <= 0) return () => 0.5;
  return (v) => Math.max(0, Math.min(1, (v - lo) / span));
}

function drawImage(
  canvas: HTMLCanvasElement,
  payload: TensorPayload,
  plan: SlicePlan,
  lo: number,
  hi: number,
  ramp: RampName,
): void {
  const { w, h } = plan;
  if (w < 1 || h < 1) return;
  canvas.width = w;
  canvas.height = h;
  const ctx = canvas.getContext("2d");
  if (!ctx) return;
  const img = ctx.createImageData(w, h);
  const px = img.data;
  const norm = normalizer(lo, hi);
  const shade = RAMPS[ramp];
  const color = new THREE.Color();
  const data = payload.data;
  const pixels = w * h;
  const rgb = plan.picks.length >= 3;
  const j0 = plan.picks[0] ?? 0;
  const j1 = plan.picks[1] ?? 0;
  const j2 = plan.picks[2] ?? 0;

  for (let p = 0; p < pixels; p += 1) {
    const o = p * 4;
    px[o + 3] = 255;
    if (rgb) {
      const r = valueAt(data, plan, p, j0);
      const g = valueAt(data, plan, p, j1);
      const b = valueAt(data, plan, p, j2);
      if (!Number.isFinite(r) || !Number.isFinite(g) || !Number.isFinite(b)) {
        px[o] = 255;
        px[o + 1] = 0;
        px[o + 2] = 255;
        continue;
      }
      px[o] = Math.round(norm(r) * 255);
      px[o + 1] = Math.round(norm(g) * 255);
      px[o + 2] = Math.round(norm(b) * 255);
      continue;
    }
    const v = valueAt(data, plan, p, j0);
    if (!Number.isFinite(v)) {
      px[o] = 255;
      px[o + 1] = 0;
      px[o + 2] = 255;
      continue;
    }
    shade(norm(v), color);
    px[o] = Math.round(Math.max(0, Math.min(1, color.r)) * 255);
    px[o + 1] = Math.round(Math.max(0, Math.min(1, color.g)) * 255);
    px[o + 2] = Math.round(Math.max(0, Math.min(1, color.b)) * 255);
  }
  ctx.putImageData(img, 0, 0);
}

function drawLine(
  canvas: HTMLCanvasElement,
  payload: TensorPayload,
  stageW: number,
  stageH: number,
  lo: number,
  hi: number,
): void {
  const w = Math.max(1, Math.floor(stageW));
  const h = Math.max(1, Math.floor(stageH));
  canvas.width = w;
  canvas.height = h;
  const ctx = canvas.getContext("2d");
  if (!ctx) return;
  ctx.clearRect(0, 0, w, h);
  const data = payload.data;
  const n = data.length;
  if (n === 0) return;

  const norm = normalizer(lo, hi);
  const yOf = (v: number) => h - 1 - norm(v) * (h - 1);

  if (lo <= 0 && hi >= 0) {
    ctx.strokeStyle = ZERO_COLOR;
    ctx.lineWidth = 1;
    ctx.beginPath();
    const y = Math.round(yOf(0)) + 0.5;
    ctx.moveTo(0, y);
    ctx.lineTo(w, y);
    ctx.stroke();
  }

  const bad: number[] = [];
  ctx.strokeStyle = LINE_COLOR;
  ctx.lineWidth = 1;
  ctx.beginPath();
  let started = false;
  for (let x = 0; x < w; x += 1) {
    const a = Math.floor((x * n) / w);
    const b = Math.min(n, Math.max(a + 1, Math.floor(((x + 1) * n) / w)));
    let mn = Infinity;
    let mx = -Infinity;
    let seenBad = false;
    for (let i = a; i < b; i += 1) {
      const v = data[i] ?? NaN;
      if (!Number.isFinite(v)) {
        seenBad = true;
        continue;
      }
      if (v < mn) mn = v;
      if (v > mx) mx = v;
    }
    if (mn === Infinity) {
      if (seenBad) bad.push(x);
      started = false;
      continue;
    }
    const px = x + 0.5;
    if (mx > mn) {
      ctx.moveTo(px, yOf(mx));
      ctx.lineTo(px, yOf(mn));
      started = false;
      continue;
    }
    const y = yOf(mn);
    if (started) ctx.lineTo(px, y);
    else ctx.moveTo(px, y);
    started = true;
  }
  ctx.stroke();

  if (bad.length > 0) {
    ctx.strokeStyle = BAD_COLOR;
    ctx.beginPath();
    for (const x of bad) {
      ctx.moveTo(x + 0.5, 0);
      ctx.lineTo(x + 0.5, h);
    }
    ctx.stroke();
  }
}

function statScalar(v: [number, number] | number | null | undefined): number | undefined {
  return typeof v === "number" ? v : undefined;
}

function shapeText(shape: readonly number[] | null): string {
  return shape && shape.length > 0 ? `(${shape.join(", ")})` : "—";
}

export function TensorView({ win, src }: PeekViewProps) {
  const shape = src.stat?.value?.shape ?? null;
  const { layout: layoutOpt, sliceIndex, channel, ramp, range } = win.opts;

  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const stageRef = useRef<HTMLDivElement | null>(null);
  const [stage, setStage] = useState({ w: 0, h: 0 });
  const [loaded, setLoaded] = useState<{ key: string; data: TensorPayload } | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [loading, setLoading] = useState(false);

  const plan = useMemo(
    () => makePlan(shape, layoutOpt, sliceIndex, channel),
    [shape, layoutOpt, sliceIndex, channel],
  );

  const blocked = src.status !== null;
  const lockedRun = win.locked?.runId ?? null;
  const runId = lockedRun ?? src.runId;
  const nodeId = src.resolved?.nodeId ?? "";
  const port = src.resolved?.port ?? "";
  const tooBig = plan.count > MAX_SLICE_ELEMENTS;
  const tooWide = plan.kind === "image" && (plan.w > MAX_EDGE || plan.h > MAX_EDGE);
  const showStage = !blocked && shape !== null && plan.kind !== "none";
  const canFetch =
    showStage &&
    plan.count > 0 &&
    !tooBig &&
    !tooWide &&
    runId !== null &&
    nodeId !== "" &&
    port !== "";

  const { offset, count } = plan;
  const fetchKey = `${runId ?? ""}|${nodeId}|${port}|${offset}|${count}`;

  useEffect(() => {
    if (!canFetch || runId === null) {
      setLoaded(null);
      setError(null);
      setLoading(false);
      return;
    }
    let cancelled = false;
    setLoading(true);
    setError(null);
    void (async () => {
      try {
        const buffer = await transport.getOutputTensor(runId, nodeId, port, offset, count);
        const decoded = decodeTensor(buffer);
        if (cancelled) return;
        setLoaded({ key: fetchKey, data: decoded });
      } catch (e) {
        if (cancelled) return;
        setLoaded(null);
        setError(lockedRun ? PEEK_FROZEN : e instanceof Error ? e.message : String(e));
      } finally {
        if (!cancelled) setLoading(false);
      }
    })();
    return () => {
      cancelled = true;
    };
  }, [canFetch, runId, lockedRun, nodeId, port, offset, count, fetchKey]);

  const payload = loaded !== null && loaded.key === fetchKey ? loaded.data : null;

  useEffect(() => registerPeekCanvas(win.id, () => canvasRef.current), [win.id]);

  useEffect(() => {
    const el = stageRef.current;
    if (!el) return;
    const observer = new ResizeObserver(() => {
      const w = el.clientWidth;
      const h = el.clientHeight;
      setStage((prev) => (prev.w === w && prev.h === h ? prev : { w, h }));
    });
    observer.observe(el);
    return () => observer.disconnect();
  }, [showStage]);

  const extent = useMemo(() => (payload ? sliceExtent(payload, plan) : null), [payload, plan]);
  const auto = range === "auto";
  const lo = auto ? (extent?.lo ?? 0) : range[0];
  const hi = auto ? (extent?.hi ?? 1) : range[1];

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas || !payload || !showStage) return;
    if (plan.kind === "line") drawLine(canvas, payload, stage.w, stage.h, lo, hi);
    else drawImage(canvas, payload, plan, lo, hi, ramp);
  }, [payload, plan, lo, hi, ramp, stage.w, stage.h, showStage]);

  const setOpts = (partial: Partial<PeekOpts>) => {
    usePeekStore.getState().setOpts(win.id, partial);
  };

  const rootProps = {
    className: "peek-tensor",
    "data-testid": "peek-tensor",
    "data-rank": plan.rank,
    "data-layout": plan.layout,
    "data-slice-count": plan.count,
  };

  if (blocked) {
    return (
      <div {...rootProps}>
        <p className="peek-tensor__msg" data-testid="peek-tensor-msg">
          {src.status}
        </p>
      </div>
    );
  }

  if (!src.stat) {
    return (
      <div {...rootProps}>
        <p className="peek-tensor__msg" data-testid="peek-tensor-msg">
          该节点尚未产出结果
        </p>
      </div>
    );
  }

  if (shape === null) {
    return (
      <div {...rootProps}>
        <p className="peek-tensor__msg" data-testid="peek-tensor-msg">
          拿不到张量形状
        </p>
      </div>
    );
  }

  const value = src.stat.value;
  if (plan.kind === "none") {
    return (
      <div {...rootProps}>
        <div className="peek-tensor__stats" data-testid="peek-tensor-stats">
          <table>
            <tbody>
              <tr>
                <th scope="row">shape</th>
                <td>{shapeText(shape)}</td>
              </tr>
              <tr>
                <th scope="row">count</th>
                <td>{typeof value?.count === "number" ? value.count : "—"}</td>
              </tr>
              <tr>
                <th scope="row">min</th>
                <td>{num(statScalar(value?.min))}</td>
              </tr>
              <tr>
                <th scope="row">max</th>
                <td>{num(statScalar(value?.max))}</td>
              </tr>
              <tr>
                <th scope="row">mean</th>
                <td>{num(value?.mean ?? undefined)}</td>
              </tr>
            </tbody>
          </table>
          <p className="peek-tensor__msg" data-testid="peek-tensor-msg">
            rank {plan.rank} 不出图，只给形状与统计量
          </p>
        </div>
      </div>
    );
  }

  let overlay: string | null = null;
  if (tooBig) overlay = `这一片太大（${plan.count} 个元素），缩小切片或换通道`;
  else if (tooWide) overlay = `这一片的边长太大（${plan.w}×${plan.h}），画不下`;
  else if (plan.count === 0) overlay = "这一片是空的";
  else if (error !== null) overlay = error;
  else if (payload === null) overlay = loading ? "正在取张量…" : "还没取到这一片";

  const total = payload?.total ?? shape.reduce((a, b) => a * b, 1);
  const got = payload?.count ?? 0;

  return (
    <div {...rootProps}>
      <div className="peek-tensor__bar" data-testid="peek-tensor-bar">
        <span className="peek-tensor__field">
          <span>布局</span>
          <select
            className="peek-tensor__select"
            data-testid="peek-tensor-layout"
            value={layoutOpt}
            disabled={lockedRun !== null}
            title={lockedRun ? PEEK_FROZEN : "按这个布局解读形状"}
            onChange={(e) => setOpts({ layout: e.target.value as TensorLayout })}
          >
            {LAYOUT_NAMES.map((name) => (
              <option key={name} value={name}>
                {name}
              </option>
            ))}
          </select>
          <span className="peek-tensor__eff" title="当前实际按这个布局读">
            {plan.layout}
          </span>
        </span>

        {plan.sliceCount > 0 && (
          <span className="peek-tensor__field">
            <span>切片</span>
            <input
              className="peek-tensor__num peek-tensor__num--idx"
              data-testid="peek-tensor-slice"
              type="number"
              min={0}
              max={plan.sliceCount - 1}
              value={plan.slice}
              disabled={lockedRun !== null}
              title={lockedRun ? PEEK_FROZEN : "第几张切片"}
              onChange={(e) => {
                const v = Number.parseInt(e.target.value, 10);
                if (!Number.isFinite(v)) return;
                setOpts({
                  sliceIndex: Math.min(Math.max(0, v), plan.sliceCount - 1),
                });
              }}
            />
            <span>/ {plan.sliceCount}</span>
          </span>
        )}

        {plan.channelCount > 1 && (
          <span className="peek-tensor__field">
            <span>通道</span>
            <select
              className="peek-tensor__select"
              data-testid="peek-tensor-channel"
              value={plan.rgb ? "rgb" : String(plan.channel)}
              disabled={lockedRun !== null}
              title={lockedRun ? PEEK_FROZEN : "看哪个通道"}
              onChange={(e) =>
                setOpts({
                  channel: e.target.value === "rgb" ? "rgb" : Number(e.target.value),
                })
              }
            >
              {Array.from({ length: plan.channelCount }, (_, i) => (
                <option key={i} value={String(i)}>
                  {i}
                </option>
              ))}
              {plan.channelCount >= 3 && <option value="rgb">rgb</option>}
            </select>
          </span>
        )}

        {plan.kind === "image" && !plan.rgb && (
          <span className="peek-tensor__field">
            <span>色带</span>
            <select
              className="peek-tensor__select"
              data-testid="peek-tensor-ramp"
              value={ramp}
              onChange={(e) => setOpts({ ramp: e.target.value as RampName })}
            >
              {RAMP_NAMES.map((name) => (
                <option key={name} value={name}>
                  {name}
                </option>
              ))}
            </select>
          </span>
        )}

        <span className="peek-tensor__field">
          <label className="peek-tensor__check">
            <input
              type="checkbox"
              data-testid="peek-tensor-auto"
              checked={auto}
              onChange={(e) =>
                setOpts({ range: e.target.checked ? "auto" : [lo, hi] })
              }
            />
            <span>自动归一化</span>
          </label>
          {!auto && (
            <>
              <input
                className="peek-tensor__num"
                data-testid="peek-tensor-lo"
                type="number"
                key={`lo-${lo}`}
                defaultValue={lo}
                onKeyDown={(e) => {
                  if (e.key === "Enter") e.currentTarget.blur();
                }}
                onBlur={(e) => {
                  const v = Number.parseFloat(e.target.value);
                  if (Number.isFinite(v)) setOpts({ range: [v, hi] });
                }}
              />
              <input
                className="peek-tensor__num"
                data-testid="peek-tensor-hi"
                type="number"
                key={`hi-${hi}`}
                defaultValue={hi}
                onKeyDown={(e) => {
                  if (e.key === "Enter") e.currentTarget.blur();
                }}
                onBlur={(e) => {
                  const v = Number.parseFloat(e.target.value);
                  if (Number.isFinite(v)) setOpts({ range: [lo, v] });
                }}
              />
            </>
          )}
        </span>
      </div>

      <div className="peek-tensor__stage" ref={stageRef}>
        <canvas
          className="peek-tensor__canvas"
          data-testid="peek-tensor-canvas"
          data-mode={plan.kind}
          data-w={plan.w}
          data-h={plan.h}
          ref={canvasRef}
        />
        {overlay !== null && (
          <p className="peek-tensor__overlay" data-testid="peek-tensor-msg">
            {overlay}
          </p>
        )}
      </div>

      <div className="peek-tensor__foot" data-testid="peek-tensor-foot">
        {shapeText(shape)} · 本片 min {num(extent?.lo)} / max {num(extent?.hi)} · 取了 {got} /{" "}
        {total} 个元素
      </div>
    </div>
  );
}
