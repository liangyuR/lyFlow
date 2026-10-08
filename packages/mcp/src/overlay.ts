import type { Picture } from "./image.js";
import { object } from "./summary.js";

type Point = [number, number];
const COLORS: Record<string, [number, number, number]> = {
  path: [0, 220, 255], nozzle: [255, 200, 0], station: [100, 200, 255],
  "edge.left": [0, 255, 100], "edge.right": [255, 100, 220], part: [150, 255, 0],
  defect: [255, 30, 30], ng: [255, 30, 30], missing: [255, 60, 40], outlier: [255, 160, 0],
};
const DIGITS = ["111101101101111", "010110010010111", "111001111100111", "111001111001111", "101101111001001", "111100111001111", "111100111101111", "111001001001001", "111101111101111", "111101111001111"];

/** Raster presentation only: all geometry and labels originate in core overlay2d records. */
export function drawOverlays(picture: Picture, records: unknown[], origin: Point, pixelScale: Point): { picture: Picture; labels: unknown[]; itemCount: number } {
  const p: Picture = { ...picture, channels: 3, bytes: new Uint8Array(picture.width * picture.height * 3) };
  for (let i = 0; i < p.width * p.height; i += 1) {
    for (let c = 0; c < 3; c += 1) p.bytes[i * 3 + c] = picture.bytes[i * picture.channels + (picture.channels === 1 ? 0 : c)]!;
  }
  const labels: unknown[] = [];
  let itemCount = 0;
  const map = (v: unknown): Point | null => Array.isArray(v) && v.length === 2 && v.every((x) => typeof x === "number" && Number.isFinite(x))
    ? [(Number(v[0]) - origin[0]) / pixelScale[0], (Number(v[1]) - origin[1]) / pixelScale[1]] : null;
  for (const raw of records) {
    const rec = object(raw);
    const data = object(rec["data"]);
    if (rec["type"] !== "lyflow.overlay2d" || data["frame"] !== "image" || !Array.isArray(data["items"])) throw new Error("叠画必须是 frame=image 的 lyflow.overlay2d Record");
    for (const rawItem of data["items"]) {
      const item = object(rawItem);
      const color = COLORS[String(item["role"])] ?? [255, 255, 0];
      const dot = (x: number, y: number) => {
        x = Math.round(x); y = Math.round(y);
        if (x < 0 || y < 0 || x >= p.width || y >= p.height) return;
        p.bytes.set(color, (y * p.width + x) * 3);
      };
      // Clip to the picture before stepping; huge off-screen coordinates cannot create an unbounded loop.
      const line = (a: Point, b: Point) => {
        let lo = 0, hi = 1;
        const dx = b[0] - a[0], dy = b[1] - a[1];
        for (const [v, q] of [[-dx, a[0]], [dx, p.width - 1 - a[0]], [-dy, a[1]], [dy, p.height - 1 - a[1]]] as Point[]) {
          if (v === 0) { if (q < 0) return; continue; }
          const t = q / v;
          if (v < 0) lo = Math.max(lo, t); else hi = Math.min(hi, t);
          if (lo > hi) return;
        }
        const x = a[0] + lo * dx, y = a[1] + lo * dy;
        const n = Math.ceil(Math.max(Math.abs((hi - lo) * dx), Math.abs((hi - lo) * dy)));
        for (let i = 0; i <= n; i += 1) dot(x + (hi - lo) * dx * (n ? i / n : 0), y + (hi - lo) * dy * (n ? i / n : 0));
      };
      const points = Array.isArray(item["points"]) ? item["points"].map(map).filter((x): x is Point => x !== null) : [];
      const kind = item["kind"];
      let anchor = points[0] ?? map(item["at"] ?? item["center"] ?? item["min"]);
      if (kind === "points") for (const [x, y] of points) { line([x - 3, y], [x + 3, y]); line([x, y - 3], [x, y + 3]); }
      if (kind === "polyline" || kind === "segments") {
        for (let i = 1; i < points.length; i += kind === "segments" ? 2 : 1) line(points[i - 1]!, points[i]!);
        if (kind === "polyline" && item["closed"] && points.length > 1) line(points[points.length - 1]!, points[0]!);
      }
      if (kind === "box") {
        const a = map(item["min"]), b = map(item["max"]);
        if (a && b) { line(a, [b[0], a[1]]); line([b[0], a[1]], b); line(b, [a[0], b[1]]); line([a[0], b[1]], a); }
      }
      if (kind === "circle") {
        const c = map(item["center"]), r = Number(item["radius"]);
        if (c && Number.isFinite(r) && r >= 0) {
          const n = Math.min(4096, Math.max(16, Math.ceil(2 * Math.PI * r / Math.min(...pixelScale))));
          let a: Point = [c[0] + r / pixelScale[0], c[1]];
          for (let i = 1; i <= n; i += 1) {
            const angle = i / n * 2 * Math.PI;
            const b: Point = [c[0] + Math.cos(angle) * r / pixelScale[0], c[1] + Math.sin(angle) * r / pixelScale[1]];
            line(a, b); a = b;
          }
        }
      }
      if (item["label"] || kind === "text") {
        const id = labels.length + 1;
        labels.push({ id, role: item["role"], text: item["text"] ?? item["label"], at: item["at"] ?? item["center"] ?? item["min"] ?? (Array.isArray(item["points"]) ? item["points"][0] : null) });
        if (anchor) for (const [column, digit] of [...String(id)].entries()) {
          const glyph = DIGITS[Number(digit)]!;
          for (let j = 0; j < glyph.length; j += 1) if (glyph[j] === "1") dot(anchor[0] + column * 4 + j % 3, anchor[1] + Math.floor(j / 3));
        }
      }
      itemCount += 1;
    }
  }
  return { picture: p, labels, itemCount };
}
