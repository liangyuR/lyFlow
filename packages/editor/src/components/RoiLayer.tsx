// 2D 剖面视图里的可拖框（m8-plan L15）。带 roi 语义标记的 vec4f 参数画成一个个框：拖框身是平移，
// 拖四个角是拉伸，松手写回参数（整段拖动一条撤销）。框是叠在 WebGL 画布上的 DOM —— 命中、光标、
// 验收脚本的真实鼠标事件都是现成的；位置每帧按正交相机重新投影，平移缩放时跟着走。

import { useCallback, useEffect, useRef, useState } from "react";
import * as THREE from "three";

import { placeLabels, type LabelBox } from "../lib/roiFrames";
import { useGraphStore } from "../store/graph";

/** Viewer3D 的场景里 RoiLayer 用得到的那几样。 */
export interface RoiHost {
  ortho: THREE.OrthographicCamera;
  renderer: THREE.WebGLRenderer;
  controls: { enabled: boolean };
  frameListeners: Set<() => void>;
}

export interface RoiItem {
  /** 参数名，也是 data-testid 的后缀。 */
  param: string;
  label: string;
  color: string;
  /** 参数值（参数自己的单位）。 */
  value: [number, number, number, number];
  /** 参数单位 → 米。mm 是 0.001。 */
  scale: number;
  /** 值退化（没填过）时画在哪（米）。 */
  placeholder: [number, number, number, number];
}

const CORNERS = ["nw", "ne", "sw", "se"] as const;
type Corner = (typeof CORNERS)[number];
type Mode = "move" | Corner;

export function isDegenerate(v: readonly number[]): boolean {
  return !(v[2]! > v[0]!) || !(v[3]! > v[1]!);
}

/** 拖动吸附：毫米吸到 0.1，米吸到 0.1 mm。手填坐标的精度也就到这儿，拖出一串浮点噪声反而难读。 */
function snap(v: number, scale: number): number {
  const step = 0.0001 / scale;
  return Math.round(v / step) * step;
}

function round(v: number): number {
  return Number(v.toFixed(6));
}

/** 世界坐标（米）→ 画布内的像素。 */
function toScreen(host: RoiHost, x: number, y: number): { x: number; y: number } {
  const el = host.renderer.domElement;
  const v = new THREE.Vector3(x, y, 0).project(host.ortho);
  return { x: ((v.x + 1) / 2) * el.clientWidth, y: ((1 - v.y) / 2) * el.clientHeight };
}

/** 一个屏幕像素是多少米。正交相机下处处相同。 */
function metersPerPixel(host: RoiHost): number {
  const el = host.renderer.domElement;
  const cam = host.ortho;
  return (cam.right - cam.left) / Math.max(cam.zoom, 1e-9) / Math.max(el.clientWidth, 1);
}

export function RoiLayer({
  host,
  nodeId,
  items,
}: {
  host: RoiHost | null;
  nodeId: string;
  items: RoiItem[];
}) {
  const boxRefs = useRef(new Map<string, HTMLDivElement>());
  const layerRef = useRef<HTMLDivElement>(null);
  // 最近动过的那个框压在最上面：框挨着框时，刚拖过来的那个的把手不能被邻居盖住
  const [active, setActive] = useState<string | null>(null);
  const itemsRef = useRef(items);
  itemsRef.current = items;
  const drag = useRef<{
    param: string;
    mode: Mode;
    start: { x: number; y: number };
    /** 起点的框，米。 */
    rect: [number, number, number, number];
    scale: number;
    label: string;
  } | null>(null);

  // 每帧按相机把框摆到位。只改 style，不走 React —— 平移缩放时一帧一次 setState 太贵。
  useEffect(() => {
    if (!host) return;
    const layout = () => {
      // 当前的「米 → 像素」仿射（每轴 s·x + o，相对本层左上角）。只读地给验收脚本：
      // 它按这个算出目标框在屏幕上的位置，再用真实鼠标去拖。
      const p0 = toScreen(host, 0, 0);
      const p1 = toScreen(host, 1, 1);
      layerRef.current?.setAttribute(
        "data-map",
        [p1.x - p0.x, p0.x, p1.y - p0.y, p0.y].map((v) => v.toFixed(3)).join(","),
      );
      const boxes: LabelBox[] = [];
      const labels: HTMLElement[] = [];
      for (const item of itemsRef.current) {
        const el = boxRefs.current.get(item.param);
        if (!el) continue;
        const r = rectMeters(item);
        const a = toScreen(host, r[0], r[3]);
        const b = toScreen(host, r[2], r[1]);
        const left = Math.min(a.x, b.x);
        const top = Math.min(a.y, b.y);
        const width = Math.max(Math.abs(b.x - a.x), 2);
        const height = Math.max(Math.abs(b.y - a.y), 2);
        el.style.left = `${left}px`;
        el.style.top = `${top}px`;
        el.style.width = `${width}px`;
        el.style.height = `${height}px`;
        const label = el.querySelector<HTMLElement>(".roi-box__label");
        if (!label) continue;
        boxes.push({ left, top, width, height, labelWidth: label.offsetWidth, labelHeight: label.offsetHeight });
        labels.push(label);
      }
      // L21：相邻框的标签互不遮挡 —— 默认放在框上方，撞上别的标签就挪到框内侧或上下错开
      placeLabels(boxes).forEach((spot, i) => {
        const label = labels[i]!;
        label.style.left = `${spot.dx}px`;
        label.style.top = `${spot.dy}px`;
        if (label.dataset.pos !== spot.where) label.dataset.pos = spot.where;
      });
    };
    host.frameListeners.add(layout);
    layout();
    return () => {
      host.frameListeners.delete(layout);
    };
  }, [host]);

  const onPointerDown = useCallback(
    (e: React.PointerEvent<HTMLElement>, item: RoiItem, mode: Mode) => {
      if (e.button !== 0 || !host) return;
      e.preventDefault();
      e.stopPropagation();
      (e.currentTarget as HTMLElement).setPointerCapture(e.pointerId);
      host.controls.enabled = false;
      setActive(item.param);
      useGraphStore.getState().begin();
      drag.current = {
        param: item.param,
        mode,
        start: { x: e.clientX, y: e.clientY },
        rect: rectMeters(item),
        scale: item.scale,
        label: item.label,
      };
    },
    [host],
  );

  const onPointerMove = useCallback(
    (e: React.PointerEvent<HTMLElement>) => {
      const d = drag.current;
      if (!d || !host) return;
      const mpp = metersPerPixel(host);
      // 屏幕 y 向下，世界 y 向上（正交相机 up = +Y）
      const dx = (e.clientX - d.start.x) * mpp;
      const dy = -(e.clientY - d.start.y) * mpp;
      let [x0, y0, x1, y1] = d.rect;
      if (d.mode === "move") {
        x0 += dx;
        x1 += dx;
        y0 += dy;
        y1 += dy;
      } else {
        if (d.mode === "nw" || d.mode === "sw") x0 += dx;
        else x1 += dx;
        if (d.mode === "nw" || d.mode === "ne") y1 += dy;
        else y0 += dy;
      }
      const lo = (a: number, b: number) => Math.min(a, b);
      const hi = (a: number, b: number) => Math.max(a, b);
      const toParam = (m: number) => round(snap(m / d.scale, d.scale));
      const next = [
        toParam(lo(x0, x1)),
        toParam(lo(y0, y1)),
        toParam(hi(x0, x1)),
        toParam(hi(y0, y1)),
      ];
      useGraphStore.getState().setParam(nodeId, d.param, next);
    },
    [host, nodeId],
  );

  const endDrag = useCallback(
    (e: React.PointerEvent<HTMLElement>) => {
      const d = drag.current;
      if (!d) return;
      drag.current = null;
      const el = e.currentTarget as HTMLElement;
      if (el.hasPointerCapture(e.pointerId)) el.releasePointerCapture(e.pointerId);
      if (host) host.controls.enabled = true;
      useGraphStore.getState().commit(`拖动 ${d.label}`);
    },
    [host],
  );

  return (
    <div
      ref={layerRef}
      className="roi-layer"
      data-testid="roi-layer"
      data-node={nodeId}
      data-count={items.length}
    >
      {items.map((item) => {
        const unset = isDegenerate(item.value);
        return (
          <div
            key={item.param}
            ref={(el) => {
              if (el) boxRefs.current.set(item.param, el);
              else boxRefs.current.delete(item.param);
            }}
            className={`roi-box${unset ? " is-unset" : ""}${active === item.param ? " is-active" : ""}`}
            style={{ ["--roi-color" as string]: item.color }}
            data-testid={`roi-box-${item.param}`}
            data-roi={item.value.join(",")}
            data-unset={unset ? "1" : "0"}
            onPointerDown={(e) => onPointerDown(e, item, "move")}
            onPointerMove={onPointerMove}
            onPointerUp={endDrag}
            onPointerCancel={endDrag}
            title={`${item.label}：拖框身平移，拖四角拉伸`}
          >
            <span className="roi-box__label" data-testid={`roi-label-${item.param}`}>
              {item.label}
              {unset ? "（未设置）" : ""}
            </span>
            {CORNERS.map((c) => (
              <span
                key={c}
                className={`roi-box__handle roi-box__handle--${c}`}
                data-testid={`roi-handle-${item.param}-${c}`}
                onPointerDown={(e) => onPointerDown(e, item, c)}
                onPointerMove={onPointerMove}
                onPointerUp={endDrag}
                onPointerCancel={endDrag}
              />
            ))}
          </div>
        );
      })}
    </div>
  );
}

/** 框在世界坐标里的矩形（米）。没填过的画在占位的地方，第一次拖动就落成真值。 */
function rectMeters(item: RoiItem): [number, number, number, number] {
  if (isDegenerate(item.value)) return item.placeholder;
  const s = item.scale;
  return [item.value[0] * s, item.value[1] * s, item.value[2] * s, item.value[3] * s];
}
