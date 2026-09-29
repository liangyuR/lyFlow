// 图像画布（docs/image-plan.md §5.4）：连线查看器的图像视图与主预览的图像模式共用这一份。
// 先按窗口能放下的大小要一级缩小的概览（core 的块均值，ABI v15），可以切到原图；滚轮缩放、拖动平移、
// 双击回到适配。悬停读出原图坐标与像素值 —— level > 0 时读到的是那一块的均值，读数上写明。
// 叠画（像素几何、可拖的像素框）经 overlay 画在图上，坐标一律是**原图像素**，与显示的级别无关。
import { useEffect, useMemo, useRef, useState, type ReactNode } from "react";
import * as THREE from "three";

import { num } from "./Inspector";
import { RAMPS, type RampName } from "../lib/ramps";
import { transport } from "../transport";
import { decodeImage, type ImagePayload } from "../types/execution";

import "../styles.peek.tensor.css";
import "../styles.peek.image.css";

/** 概览级别的长边上限：再大浏览器的 canvas 也画得动，但 IPC 与上色都开始按秒计。 */
const OVERVIEW_EDGE = 2048;
/** 手动选级别时，这一级的像素数上限（约 4096²）。原图更大就只能看缩小的。 */
const MAX_LEVEL_PIXELS = 16_777_216;
const RAMP_NAMES: RampName[] = ["gray", "viridis", "jet"];

export type Depth = "u8" | "u16" | "f32";

/** 与 core 的 shrinkImage 同一个取整：尺寸向上取整。 */
function levelSize(full: number, level: number): number {
  const block = 2 ** level;
  return Math.max(1, Math.ceil(full / block));
}

/** 长边不超过 edge 的最小级别。 */
function levelToFit(w: number, h: number, edge: number): number {
  let level = 0;
  while (Math.max(levelSize(w, level), levelSize(h, level)) > edge && level < 30) level += 1;
  return level;
}

function maxLevelOf(w: number, h: number): number {
  let level = 0;
  while (2 ** level < Math.max(w, h) && level < 30) level += 1;
  return level;
}

/** 不拉伸时每种位深的「原值」范围：u8 0..255、u16 0..65535、f32 0..1。 */
function naturalRange(depth: Depth): [number, number] {
  if (depth === "u8") return [0, 255];
  if (depth === "u16") return [0, 65535];
  return [0, 1];
}

interface Loaded {
  key: string;
  /** 不含 runId 的那一半：同一个节点、端口、级别。新一次运行的图还没到时，旧的先顶着。 */
  viewKey: string;
  width: number;
  height: number;
  channels: number;
  level: number;
  pixels: Uint8Array | Uint16Array | Float32Array;
}

function extentOf(img: Loaded, picks: number[]): { lo: number; hi: number } | null {
  let lo = Infinity;
  let hi = -Infinity;
  const { pixels, channels } = img;
  const n = img.width * img.height;
  for (let p = 0; p < n; p += 1) {
    for (const c of picks) {
      const v = pixels[p * channels + c] ?? NaN;
      if (!Number.isFinite(v)) continue;
      if (v < lo) lo = v;
      if (v > hi) hi = v;
    }
  }
  return lo === Infinity ? null : { lo, hi };
}

function draw(
  canvas: HTMLCanvasElement,
  img: Loaded,
  picks: number[],
  lo: number,
  hi: number,
  ramp: RampName,
): void {
  canvas.width = img.width;
  canvas.height = img.height;
  const ctx = canvas.getContext("2d");
  if (!ctx) return;
  const out = ctx.createImageData(img.width, img.height);
  const px = out.data;
  const span = hi - lo;
  const norm =
    Number.isFinite(span) && span > 0
      ? (v: number) => Math.max(0, Math.min(1, (v - lo) / span))
      : () => 0.5;
  const shade = RAMPS[ramp];
  const color = new THREE.Color();
  const { pixels, channels } = img;
  const n = img.width * img.height;
  const rgb = picks.length >= 3;
  for (let p = 0; p < n; p += 1) {
    const o = p * 4;
    const base = p * channels;
    px[o + 3] = 255;
    if (rgb) {
      const r = pixels[base + picks[0]!] ?? NaN;
      const g = pixels[base + picks[1]!] ?? NaN;
      const b = pixels[base + picks[2]!] ?? NaN;
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
    const v = pixels[base + picks[0]!] ?? NaN;
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
  ctx.putImageData(out, 0, 0);
}

/** 一级图像按段取齐：桥接层按 16 MB 收行数，照帧头的 rowCount 接着要。 */
async function fetchLevel(
  runId: string,
  nodeId: string,
  port: string,
  level: number,
): Promise<Omit<Loaded, "key" | "viewKey">> {
  const get = transport.getOutputImage;
  if (!get) throw new Error("宿主不支持取图像（需要 C ABI v15）");
  let first: ImagePayload | null = null;
  let pixels: Uint8Array | Uint16Array | Float32Array | null = null;
  let row = 0;
  for (;;) {
    const part = decodeImage(await get.call(transport, runId, nodeId, port, level, row, 0));
    if (first === null) {
      first = part;
      const n = part.width * part.height * part.channels;
      pixels =
        part.depth === 1 ? new Uint8Array(n) : part.depth === 2 ? new Uint16Array(n) : new Float32Array(n);
    }
    if (part.rowCount === 0) break;
    pixels!.set(part.pixels, part.rowOffset * part.width * part.channels);
    row = part.rowOffset + part.rowCount;
    if (row >= part.height) break;
  }
  if (first === null || pixels === null) throw new Error("没取到图像");
  return {
    width: first.width,
    height: first.height,
    channels: first.channels,
    level: first.level,
    pixels,
  };
}

interface ViewXf {
  scale: number;
  tx: number;
  ty: number;
}

/** 叠画拿到的坐标换算。坐标都是**原图像素**（级别 0），左上角原点、y 向下。 */
export interface ImageOverlayView {
  toStage(x: number, y: number): [number, number];
  /** client 坐标 → 原图像素（浮点，不截断、不夹到图内）。 */
  toImage(clientX: number, clientY: number): [number, number];
  /** 原图一个像素在屏幕上有多大。 */
  scale: number;
  fullWidth: number;
  fullHeight: number;
}

export interface ImageCanvasProps {
  /** 取哪一次运行的哪个输出。runId 为 null 时不取（上层显示原因）。 */
  runId: string | null;
  nodeId: string;
  port: string;
  fullW: number;
  fullH: number;
  channels: number;
  depth: Depth;
  /** 取不到时显示它而不是后端的原话（锁定快照：旧 run 的索引已被放掉）。 */
  errorText?: string | null;
  /** data-testid 的前缀：连线查看器是 peek-image，主预览是 viewer-image。 */
  testid: string;
  /** 画布挂上 / 摘下时回调（连线查看器的导出 PNG 要拿它）。 */
  onCanvas?: (canvas: HTMLCanvasElement | null) => void;
  overlay?: (view: ImageOverlayView) => ReactNode;
}

export function ImageCanvas({
  runId,
  nodeId,
  port,
  fullW,
  fullH,
  channels,
  depth,
  errorText = null,
  testid,
  onCanvas,
  overlay,
}: ImageCanvasProps) {
  const [levelChoice, setLevelChoice] = useState<"auto" | number>("auto");
  // null = 还没手动选过：跟着图走（三通道以上看 RGB；u8 不拉伸，u16 / f32 拉伸）
  const [channelPick, setChannelChoice] = useState<number | "rgb" | null>(null);
  const [ramp, setRamp] = useState<RampName>("gray");
  const [stretchPick, setStretch] = useState<boolean | null>(null);
  const channelChoice = channelPick ?? (channels >= 3 ? "rgb" : 0);
  const stretch = stretchPick ?? depth !== "u8";
  const [loaded, setLoaded] = useState<Loaded | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [loading, setLoading] = useState(false);
  const [xf, setXf] = useState<ViewXf | null>(null);
  const [hover, setHover] = useState<{ x: number; y: number } | null>(null);

  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const stageRef = useRef<HTMLDivElement | null>(null);
  const dragRef = useRef<{ x: number; y: number; tx: number; ty: number } | null>(null);

  const autoLevel = levelToFit(fullW, fullH, OVERVIEW_EDGE);
  const maxLevel = maxLevelOf(fullW, fullH);
  let minLevel = 0;
  while (levelSize(fullW, minLevel) * levelSize(fullH, minLevel) > MAX_LEVEL_PIXELS) minLevel += 1;
  const level = levelChoice === "auto" ? autoLevel : Math.max(minLevel, Math.min(maxLevel, levelChoice));

  const canFetch = fullW > 0 && fullH > 0 && runId !== null && nodeId !== "" && port !== "";
  const viewKey = `${nodeId}|${port}|${level}`;
  const fetchKey = `${runId ?? ""}|${viewKey}`;

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
        const img = await fetchLevel(runId, nodeId, port, level);
        if (cancelled) return;
        setLoaded({ ...img, key: fetchKey, viewKey });
      } catch (e) {
        if (cancelled) return;
        setLoaded(null);
        setError(errorText ?? (e instanceof Error ? e.message : String(e)));
      } finally {
        if (!cancelled) setLoading(false);
      }
    })();
    return () => {
      cancelled = true;
    };
  }, [canFetch, runId, errorText, nodeId, port, level, fetchKey, viewKey]);

  // 自动运行 / 拖参数时的预览运行每次都换 runId：同一个节点、端口、级别的旧图先顶着，
  // 新图到了再换 —— 否则每跑一次画面就闪一下、放大的位置也丢了，拖框拖到一半还会被卸载
  const img = loaded !== null && (loaded.key === fetchKey || loaded.viewKey === viewKey) ? loaded : null;
  const stale = img !== null && img.key !== fetchKey;

  const picks = useMemo(() => {
    if (channels >= 3 && channelChoice === "rgb") return [0, 1, 2];
    const c = typeof channelChoice === "number" ? channelChoice : 0;
    return [Math.min(Math.max(0, c), Math.max(0, channels - 1))];
  }, [channels, channelChoice]);

  const extent = useMemo(() => (img ? extentOf(img, picks) : null), [img, picks]);
  const [lo, hi] = stretch && extent ? [extent.lo, extent.hi] : naturalRange(depth);


  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas || !img) return;
    draw(canvas, img, picks, lo, hi, ramp);
  }, [img, picks, lo, hi, ramp]);

  // 换了一级、换了一张图（尺寸变了）才适配到窗口；同一张图的新一次运行不动视角
  const fit = () => {
    const stage = stageRef.current;
    if (!stage || !img) return;
    const sw = stage.clientWidth;
    const sh = stage.clientHeight;
    const scale = Math.min(sw / img.width, sh / img.height);
    setXf({ scale, tx: (sw - img.width * scale) / 2, ty: (sh - img.height * scale) / 2 });
  };
  const fitKey = img ? `${img.viewKey}|${img.width}x${img.height}` : "";
  useEffect(fit, [fitKey]);

  const toImage = (clientX: number, clientY: number) => {
    const stage = stageRef.current;
    if (!stage || !xf || !img) return null;
    const r = stage.getBoundingClientRect();
    const x = Math.floor((clientX - r.left - xf.tx) / xf.scale);
    const y = Math.floor((clientY - r.top - xf.ty) / xf.scale);
    if (x < 0 || y < 0 || x >= img.width || y >= img.height) return null;
    return { x, y };
  };

  /** 原图像素 ↔ 屏幕：显示的是第 level 级，一个显示像素 = block 个原图像素。 */
  const overlayView = (t: ViewXf, block: number): ImageOverlayView => {
    const s = t.scale / block;
    return {
      scale: s,
      fullWidth: fullW,
      fullHeight: fullH,
      toStage: (x, y) => [t.tx + x * s, t.ty + y * s],
      toImage: (clientX, clientY) => {
        const r = stageRef.current?.getBoundingClientRect();
        return [(clientX - (r?.left ?? 0) - t.tx) / s, (clientY - (r?.top ?? 0) - t.ty) / s];
      },
    };
  };

  const onWheel = (e: React.WheelEvent) => {
    const stage = stageRef.current;
    if (!stage || !xf) return;
    const r = stage.getBoundingClientRect();
    const mx = e.clientX - r.left;
    const my = e.clientY - r.top;
    const k = e.deltaY < 0 ? 1.25 : 0.8;
    const scale = Math.max(0.02, Math.min(64, xf.scale * k));
    const f = scale / xf.scale;
    setXf({ scale, tx: mx - (mx - xf.tx) * f, ty: my - (my - xf.ty) * f });
  };

  if (fullW <= 0 || fullH <= 0) {
    return (
      <div className="peek-image" data-testid={testid}>
        <p className="peek-tensor__msg" data-testid={`${testid}-msg`}>
          拿不到图像尺寸
        </p>
      </div>
    );
  }

  let message: string | null = null;
  if (error !== null) message = error;
  else if (img === null) message = loading ? "正在取图像…" : "还没取到图像";

  const block = img ? 2 ** img.level : 1;
  const readout = (() => {
    if (!hover || !img) return null;
    const base = (hover.y * img.width + hover.x) * img.channels;
    const vals = Array.from({ length: img.channels }, (_, c) => img.pixels[base + c] ?? NaN);
    const x0 = hover.x * block;
    const y0 = hover.y * block;
    const where = block > 1 ? `(${x0}–${Math.min(fullW, x0 + block) - 1}, ${y0}–${Math.min(fullH, y0 + block) - 1})` : `(${x0}, ${y0})`;
    const text = vals.map((v) => (depth === "f32" ? num(v) : Number.isFinite(v) ? String(v) : "—")).join(", ");
    return `${where} = [${text}]${block > 1 ? `（第 ${img.level} 级的块均值）` : ""}`;
  })();

  const levels: number[] = [];
  for (let l = minLevel; l <= maxLevel; l += 1) levels.push(l);

  return (
    <div
      className="peek-image"
      data-testid={testid}
      data-full-w={fullW}
      data-full-h={fullH}
      data-channels={channels}
      data-depth={depth}
      data-level={img?.level ?? ""}
      data-stale={stale ? "1" : "0"}
      data-w={img?.width ?? 0}
      data-h={img?.height ?? 0}
    >
      <div className="peek-tensor__bar" data-testid={`${testid}-bar`}>
        <span className="peek-tensor__field">
          <span>级别</span>
          <select
            className="peek-tensor__select"
            data-testid={`${testid}-level`}
            value={String(levelChoice)}
            onChange={(e) => setLevelChoice(e.target.value === "auto" ? "auto" : Number(e.target.value))}
            title="0 = 原图；k = 2^k 倍块均值缩小（离远了看同一张图）"
          >
            <option value="auto">适配（{autoLevel}）</option>
            {levels.map((l) => (
              <option key={l} value={String(l)}>
                {l === 0 ? "原图" : `1/${2 ** l}`}
              </option>
            ))}
          </select>
        </span>

        {channels > 1 && (
          <span className="peek-tensor__field">
            <span>通道</span>
            <select
              className="peek-tensor__select"
              data-testid={`${testid}-channel`}
              value={picks.length >= 3 ? "rgb" : String(picks[0])}
              onChange={(e) => setChannelChoice(e.target.value === "rgb" ? "rgb" : Number(e.target.value))}
            >
              {channels >= 3 && <option value="rgb">RGB</option>}
              {Array.from({ length: channels }, (_, i) => (
                <option key={i} value={String(i)}>
                  {"RGBA"[i] ?? i}
                </option>
              ))}
            </select>
          </span>
        )}

        {picks.length === 1 && (
          <span className="peek-tensor__field">
            <span>色带</span>
            <select
              className="peek-tensor__select"
              data-testid={`${testid}-ramp`}
              value={ramp}
              onChange={(e) => setRamp(e.target.value as RampName)}
            >
              {RAMP_NAMES.map((name) => (
                <option key={name} value={name}>
                  {name}
                </option>
              ))}
            </select>
          </span>
        )}

        <label className="peek-tensor__check" title={`不拉伸时按 ${depth} 的原值范围 ${naturalRange(depth).join("–")} 显示`}>
          <input
            type="checkbox"
            data-testid={`${testid}-stretch`}
            checked={stretch}
            onChange={(e) => setStretch(e.target.checked)}
          />
          <span>拉伸到本图范围</span>
        </label>
      </div>

      <div
        className="peek-image__stage"
        ref={stageRef}
        onWheel={onWheel}
        onPointerDown={(e) => {
          if (!xf) return;
          e.currentTarget.setPointerCapture(e.pointerId);
          dragRef.current = { x: e.clientX, y: e.clientY, tx: xf.tx, ty: xf.ty };
        }}
        onPointerMove={(e) => {
          const d = dragRef.current;
          if (d && xf) {
            setXf({ ...xf, tx: d.tx + e.clientX - d.x, ty: d.ty + e.clientY - d.y });
            return;
          }
          setHover(toImage(e.clientX, e.clientY));
        }}
        onPointerUp={() => {
          dragRef.current = null;
        }}
        onPointerLeave={() => setHover(null)}
        onDoubleClick={fit}
      >
        <canvas
          className="peek-image__canvas"
          data-testid={`${testid}-canvas`}
          ref={(el) => {
            canvasRef.current = el;
            onCanvas?.(el);
          }}
          style={
            xf && img
              ? {
                  width: img.width * xf.scale,
                  height: img.height * xf.scale,
                  transform: `translate(${xf.tx}px, ${xf.ty}px)`,
                }
              : { visibility: "hidden" }
          }
        />
        {xf && img && overlay && (
          <div className="peek-image__overlay">{overlay(overlayView(xf, 2 ** img.level))}</div>
        )}
        {message !== null && (
          <p className="peek-tensor__overlay" data-testid={`${testid}-msg`}>
            {message}
          </p>
        )}
      </div>

      <div className="peek-tensor__foot" data-testid={`${testid}-foot`}>
        {readout !== null ? (
          <span data-testid={`${testid}-readout`}>{readout}</span>
        ) : (
          <>
            {fullW}×{fullH}×{channels} {depth}
            {img && img.level > 0 ? ` · 显示 1/${block}（${img.width}×${img.height}）` : ""} · 范围 {num(lo)}–{num(hi)}
          </>
        )}
      </div>
    </div>
  );
}
