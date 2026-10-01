// 图像画布（docs/image-plan.md §5.4）：连线查看器的图像视图与主预览的图像模式共用这一份。
// 先按窗口能放下的大小要一级缩小的概览（core 的块均值，ABI v15），可以切到原图；滚轮缩放、拖动平移、
// 双击回到适配。悬停读出原图坐标与像素值 —— level > 0 时读到的是那一块的均值，读数上写明。
// 叠画（像素几何、可拖的像素框）经 overlay 画在图上，坐标一律是**原图像素**，与显示的级别无关。
// 预览时源头缩小过的图（pixelScale > 1，ADR-0028）按原图尺寸摆放：一个像素拉成 pixelScale 个原图像素那么大。
// 视角（缩放、平移）与级别都按原图坐标记：换级别、预览 ↔ 正式只换画上去的那张图，视角不动。
import { useEffect, useMemo, useRef, useState, type ReactNode } from "react";
import * as THREE from "three";

import { num } from "./Inspector";
import { RAMPS, type RampName } from "../lib/ramps";
import { fetchImageLevel } from "../lib/imageFetch";
import { transport } from "../transport";

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

/** 2 的幂 s 是 2 的几次方（s = 1、2、4…）。 */
function log2Of(s: number): number {
  let k = 0;
  while (2 ** k < s && k < 30) k += 1;
  return k;
}

/** 原图坐标里的一张图：认「是不是同一张图」用，与哪一次运行、取的哪一级无关。 */
interface Footprint {
  nodeId: string;
  port: string;
  /** 原图像素：这张图自己的宽高 × 它的比例。 */
  w: number;
  h: number;
  /** 这张图的比例（预览缩小过的 > 1）：尺寸只精确到这么多个原图像素。 */
  scale: number;
}

/** 同一个节点、端口，原图坐标下的尺寸差不到粗的那一边的比例。4101 宽的原图缩一半是 2051、
 *  按原图算 4102 —— 差的那一个像素是取整，不是另一张图。 */
function sameFootprint(a: Footprint, b: Footprint): boolean {
  const m = Math.max(a.scale, b.scale);
  return a.nodeId === b.nodeId && a.port === b.port && Math.abs(a.w - b.w) < m && Math.abs(a.h - b.h) < m;
}

/** 不拉伸时每种位深的「原值」范围：u8 0..255、u16 0..65535、f32 0..1。 */
function naturalRange(depth: Depth): [number, number] {
  if (depth === "u8") return [0, 255];
  if (depth === "u16") return [0, 65535];
  return [0, 1];
}

interface Loaded {
  /** 哪一次运行的哪一级：与当前的 fetchKey 相同才是「要的那张」。 */
  key: string;
  /** 原图坐标里的这张图。新一次运行、另一级的图还没到时，同一张图的旧的先顶着。 */
  footprint: Footprint;
  /** 取的时候那张图的比例：一个显示像素 = 2^level × pixelScale 个原图像素。 */
  pixelScale: number;
  width: number;
  height: number;
  channels: number;
  level: number;
  pixels: Uint8Array | Uint16Array | Float32Array;
}

/** 一个显示像素是几个原图像素。 */
function blockOf(img: Loaded): number {
  return 2 ** img.level * img.pixelScale;
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

/** 一级图像按段取齐（lib/imageFetch）。 */
async function fetchLevel(
  runId: string,
  nodeId: string,
  port: string,
  level: number,
  abandoned: () => boolean,
): Promise<Omit<Loaded, "key" | "footprint" | "pixelScale">> {
  const get = transport.getOutputImage;
  if (!get) throw new Error("宿主不支持取图像（需要 C ABI v15）");
  return fetchImageLevel((...args) => get.apply(transport, args), runId, nodeId, port, level, abandoned);
}

/** 视角按原图坐标记：scale = 原图一个像素在屏幕上多大，(tx, ty) = 原图 (0, 0) 在舞台上的位置。
 *  换级别、预览 ↔ 正式（一个显示像素对应的原图像素数变了）都不用动它。 */
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
  /** 预览时源头缩小过的图（valueJson 的 scale，ADR-0028）：一个像素对应原图 pixelScale × pixelScale 个像素。
   *  fullW / fullH 是这张图自己的尺寸；叠画、拖框与读数照旧用原图坐标，角标写明是预览。 */
  pixelScale?: number;
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
  pixelScale = 1,
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
  /** 鼠标下的原图坐标（浮点）：落在哪个显示像素按画着的那张图算，换了一级读数跟着对。 */
  const [hover, setHover] = useState<{ x: number; y: number } | null>(null);

  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const stageRef = useRef<HTMLDivElement | null>(null);
  const dragRef = useRef<{ x: number; y: number; tx: number; ty: number } | null>(null);

  // 级别按原图算：k = 一个显示像素是 2^k 个原图像素（手选的也记成这个）。预览缩小过的图（s = 2^sk）
  // 只有 k ≥ sk 的那些：选了更细的先给它自己最细的一级，松手后的正式结果到了再是选的那一级
  const logicalW = fullW * pixelScale;
  const logicalH = fullH * pixelScale;
  const autoLevel = levelToFit(logicalW, logicalH, OVERVIEW_EDGE);
  const maxLevel = maxLevelOf(logicalW, logicalH);
  let minLevel = 0;
  while (levelSize(logicalW, minLevel) * levelSize(logicalH, minLevel) > MAX_LEVEL_PIXELS) minLevel += 1;
  const wanted = levelChoice === "auto" ? autoLevel : Math.max(minLevel, Math.min(maxLevel, levelChoice));
  /** 这张图自己要取的那一级。 */
  const level = Math.max(0, wanted - log2Of(pixelScale));
  /** 「适配」此刻落在哪一块上（下拉框里写这个，与别的选项同一种写法）。 */
  const autoBlock = 2 ** Math.max(autoLevel, log2Of(pixelScale));

  const canFetch = fullW > 0 && fullH > 0 && runId !== null && nodeId !== "" && port !== "";
  const footprint: Footprint = { nodeId, port, w: logicalW, h: logicalH, scale: pixelScale };
  const fetchKey = `${runId ?? ""}|${nodeId}|${port}|${pixelScale}|${level}`;

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
        const img = await fetchLevel(runId, nodeId, port, level, () => cancelled);
        if (cancelled) return;
        const at: Footprint = { nodeId, port, w: fullW * pixelScale, h: fullH * pixelScale, scale: pixelScale };
        setLoaded({ ...img, key: fetchKey, footprint: at, pixelScale });
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
  }, [canFetch, runId, errorText, nodeId, port, level, fetchKey, pixelScale, fullW, fullH]);

  // 自动运行 / 拖参数时的预览运行每次都换 runId，换级别也要重取：原图坐标里是同一张图的旧图先顶着，
  // 新图到了再换 —— 否则每跑一次画面就闪一下、放大的位置也丢了，拖框拖到一半还会被卸载
  const img =
    loaded !== null && (loaded.key === fetchKey || sameFootprint(loaded.footprint, footprint)) ? loaded : null;
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

  // 第一次有图、换了一张图（节点、端口或原图坐标下的尺寸变了）才适配到窗口；同一张图换了运行、换了级别、
  // 预览 ↔ 正式都不动视角（review 第二轮：按显示比例认图时，小一些的图与手选的级别每次切换都重新适配）
  const fittedFor = useRef<Footprint | null>(null);
  const fitTo = (target: Loaded) => {
    const stage = stageRef.current;
    if (!stage) return;
    // 画出来的范围（原图像素）：最后一块不满也按整块画，适配的是整张画布
    const dw = target.width * blockOf(target);
    const dh = target.height * blockOf(target);
    const sw = stage.clientWidth;
    const sh = stage.clientHeight;
    const scale = Math.min(sw / dw, sh / dh);
    setXf({ scale, tx: (sw - dw * scale) / 2, ty: (sh - dh * scale) / 2 });
    fittedFor.current = target.footprint;
  };
  useEffect(() => {
    if (!img) return;
    const prev = fittedFor.current;
    if (prev !== null && sameFootprint(prev, img.footprint)) {
      fittedFor.current = img.footprint;
      return;
    }
    fitTo(img);
    // fitTo 只读 ref 与 img
  }, [img]);

  const toImage = (clientX: number, clientY: number) => {
    const stage = stageRef.current;
    if (!stage || !xf) return null;
    const r = stage.getBoundingClientRect();
    return { x: (clientX - r.left - xf.tx) / xf.scale, y: (clientY - r.top - xf.ty) / xf.scale };
  };

  /** 原图像素 ↔ 屏幕：视角本来就按原图坐标记，与显示的是哪一级无关。 */
  const overlayView = (t: ViewXf): ImageOverlayView => ({
    scale: t.scale,
    fullWidth: logicalW,
    fullHeight: logicalH,
    toStage: (x, y) => [t.tx + x * t.scale, t.ty + y * t.scale],
    toImage: (clientX, clientY) => {
      const r = stageRef.current?.getBoundingClientRect();
      return [(clientX - (r?.left ?? 0) - t.tx) / t.scale, (clientY - (r?.top ?? 0) - t.ty) / t.scale];
    },
  });

  const onWheel = (e: React.WheelEvent) => {
    const stage = stageRef.current;
    if (!stage || !xf) return;
    const r = stage.getBoundingClientRect();
    const mx = e.clientX - r.left;
    const my = e.clientY - r.top;
    const k = e.deltaY < 0 ? 1.25 : 0.8;
    // 原图一个像素最多放到 64 屏幕像素；缩小几乎不设下限（上万像素宽的图适配时就在 0.1 以下）
    const scale = Math.max(1e-3, Math.min(64, xf.scale * k));
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

  const block = img ? blockOf(img) : 1;
  const readout = (() => {
    if (!hover || !img) return null;
    const px = Math.floor(hover.x / block);
    const py = Math.floor(hover.y / block);
    if (px < 0 || py < 0 || px >= img.width || py >= img.height) return null;
    const base = (py * img.width + px) * img.channels;
    const vals = Array.from({ length: img.channels }, (_, c) => img.pixels[base + c] ?? NaN);
    const x0 = px * block;
    const y0 = py * block;
    const { w: shownW, h: shownH } = img.footprint;
    const where = block > 1 ? `(${x0}–${Math.min(shownW, x0 + block) - 1}, ${y0}–${Math.min(shownH, y0 + block) - 1})` : `(${x0}, ${y0})`;
    const text = vals.map((v) => (depth === "f32" ? num(v) : Number.isFinite(v) ? String(v) : "—")).join(", ");
    const notes = [
      img.pixelScale > 1 ? `预览 1/${img.pixelScale}` : "",
      img.level > 0 ? `1/${block} 的块均值` : "",
    ].filter(Boolean);
    return `${where} = [${text}]${notes.length > 0 ? `（${notes.join("，")}）` : ""}`;
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
      data-block={img ? block : ""}
      data-pixel-scale={pixelScale}
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
            value={levelChoice === "auto" ? "auto" : String(wanted)}
            onChange={(e) => setLevelChoice(e.target.value === "auto" ? "auto" : Number(e.target.value))}
            title={
              pixelScale > 1
                ? `这是缩小 1/${pixelScale} 的预览图：比 1/${pixelScale} 细的级别先按 1/${pixelScale} 显示，松手后的正式结果再按选的来`
                : "原图 = 一个像素一个像素地看；1/k = k × k 块均值缩小（离远了看同一张图）"
            }
          >
            <option value="auto">适配（{autoBlock === 1 ? "原图" : `1/${autoBlock}`}）</option>
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
        onDoubleClick={() => {
          if (img) fitTo(img);
        }}
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
                  width: img.width * block * xf.scale,
                  height: img.height * block * xf.scale,
                  transform: `translate(${xf.tx}px, ${xf.ty}px)`,
                }
              : { visibility: "hidden" }
          }
        />
        {xf && img && overlay && (
          <div className="peek-image__overlay">{overlay(overlayView(xf))}</div>
        )}
        {/* 浮在画面角上，不占工具条：预览 ↔ 正式之间切换时画面不该挪动（e2e 量的就是框的屏幕位置） */}
        {pixelScale > 1 && (
          <span
            className="peek-image__badge"
            data-testid={`${testid}-preview-scale`}
            title="拖参数时的预览：源头的大图按比例缩小过，框与几何照旧是原图坐标；松手后的正式运行是原图"
          >
            预览 1/{pixelScale}
          </span>
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
            {fullW * pixelScale}×{fullH * pixelScale}×{channels} {depth}
            {pixelScale > 1 ? ` · 预览 1/${pixelScale}（${fullW}×${fullH}）` : ""}
            {img && block > 1 ? ` · 显示 1/${block}（${img.width}×${img.height}）` : ""} · 范围 {num(lo)}–{num(hi)}
          </>
        )}
      </div>
    </div>
  );
}
