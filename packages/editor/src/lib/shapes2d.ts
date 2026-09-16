// --------------------------------------------------------- 2D 几何叠画（G7）

import * as THREE from "three";

import type { OutputStat } from "../types/execution";

/** 叠画一条折线/线段集合。z 全部为 0：这些几何本来就定义在 XY 平面上。 */
export function polyline(points: number[], color: number, loop: boolean): THREE.Line {
  const geometry = new THREE.BufferGeometry();
  geometry.setAttribute("position", new THREE.BufferAttribute(new Float32Array(points), 3));
  const material = new THREE.LineBasicMaterial({ color, depthTest: false, transparent: true });
  const line = loop ? new THREE.LineLoop(geometry, material) : new THREE.Line(geometry, material);
  line.renderOrder = 10; // 永远画在点云之上，否则细线会被点糊掉
  return line;
}

export function disposeOverlay(group: THREE.Group) {
  for (const child of [...group.children]) {
    group.remove(child);
    const line = child as THREE.Line;
    line.geometry?.dispose();
    const m = line.material as THREE.Material | THREE.Material[];
    if (Array.isArray(m)) m.forEach((x) => x.dispose());
    else m?.dispose();
  }
}

/** 这个几何自己有多大。没有点云做尺度参照时拿它当兜底。 */
export function extentOf(out: OutputStat): number {
  const v = out.value;
  if (!v) return 0;
  if (v.kind === "Box2D" && Array.isArray(v.min) && Array.isArray(v.max)) {
    return Math.max(Math.abs(v.max[0] - v.min[0]), Math.abs(v.max[1] - v.min[1]));
  }
  if (v.kind === "Circle2D") return (v.radius ?? 0) * 4;
  if (v.kind === "Line2D" && v.hasSegment && v.start && v.end) {
    return Math.hypot(v.end[0] - v.start[0], v.end[1] - v.start[1]);
  }
  return 0;
}

/** 一个输出值 → 若干条线。认不出的 kind 返回空数组（前端不硬编码算子，也不该硬编码到崩）。 */
export function shapesOf(out: OutputStat, color: number, span: number): THREE.Line[] {
  const v = out.value;
  if (!v) return [];
  switch (v.kind) {
    case "Box2D": {
      const [x0, y0] = Array.isArray(v.min) ? v.min : [0, 0];
      const [x1, y1] = Array.isArray(v.max) ? v.max : [0, 0];
      return [polyline([x0, y0, 0, x1, y0, 0, x1, y1, 0, x0, y1, 0], color, true)];
    }
    case "Line2D": {
      if (v.hasSegment && v.start && v.end) {
        return [polyline([v.start[0], v.start[1], 0, v.end[0], v.end[1], 0], color, false)];
      }
      const [px, py] = v.point ?? [0, 0];
      const [dx, dy] = v.dir ?? [1, 0];
      const n = Math.hypot(dx, dy) || 1;
      const h = span / 2;
      return [
        polyline(
          [px - (dx / n) * h, py - (dy / n) * h, 0, px + (dx / n) * h, py + (dy / n) * h, 0],
          color,
          false,
        ),
      ];
    }
    case "Circle2D": {
      const [cx, cy] = v.center ?? [0, 0];
      const r = v.radius ?? 0;
      const pts: number[] = [];
      const SEGMENTS = 72;
      for (let i = 0; i < SEGMENTS; i += 1) {
        const t = (i / SEGMENTS) * Math.PI * 2;
        pts.push(cx + r * Math.cos(t), cy + r * Math.sin(t), 0);
      }
      return [polyline(pts, color, true)];
    }
    case "Point2D": {
      const [x, y] = v.p ?? [0, 0];
      // 十字而不是一个点：单个 Point 在细线材质下根本看不见
      const s = span * 0.01 || 0.001;
      return [
        polyline([x - s, y, 0, x + s, y, 0], color, false),
        polyline([x, y - s, 0, x, y + s, 0], color, false),
      ];
    }
    default:
      return [];
  }
}
