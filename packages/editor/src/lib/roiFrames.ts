// 2D 拖框的「底图组」（m8-plan L15 / L20）：带 roi 语义标记的参数按 roiBackdrop 分组，同一组的框
// 在同一片底图的坐标系里（locate_template：一个模板槽的四个框 = 一组，底图是那个槽的左右模板）。
// 视图一次只画一组，切换条列出所有组；Inspector 里所选组所在的参数组展开。纯逻辑，不碰 DOM。

import type { GraphNode } from "../types/graph";
import type { OperatorDesc, Param } from "../types/manifest";
import { effectiveParams, isVisible } from "./params";

export interface RoiFrame {
  /** 组的标识：底图 = `<dir 参数>|<files 参数…>`，数据坐标系 = "data"。 */
  key: string;
  /** 切换条上的名字，例如「模板 2 · f2」。数据坐标系的组是空串。 */
  label: string;
  /** 这一组里当前可见的框，按声明顺序。 */
  params: Param[];
  /** 底图文件（已拼好目录）。空 = 画在数据云上。 */
  files: string[];
}

export function frameKeyOf(p: Param): string {
  const b = p.roiBackdrop;
  return b ? `${b.dir}|${(b.files ?? []).join(",")}` : "data";
}

function joinPath(dir: string, file: string): string {
  if (/^[a-zA-Z]:[\\/]/.test(file) || file.startsWith("/") || file.startsWith("\\\\")) return file;
  return /[\\/]$/.test(dir) ? dir + file : `${dir}/${file}`;
}

function isRoi(p: Param): boolean {
  return p.semantic === "roi" && p.type === "vec4f";
}

/** 选中节点可拖的框，按底图分组。坐标系不同的框不能画在同一片底图上：有带底图的组时只列
 *  带底图的（locate_template 的各个槽；整体框在数据坐标系里，不混进来），否则就是数据坐标系那一组。
 *  组内只算当前可见的参数 —— 没启用的槽整组不出现（L20「只列启用的槽」）。 */
export function roiFramesOf(op: OperatorDesc | undefined, node: GraphNode | undefined): RoiFrame[] {
  if (!op || !node) return [];
  const eff = effectiveParams(op, node);
  const rois = op.params.filter((p) => isRoi(p) && isVisible(p, eff));
  if (rois.length === 0) return [];
  const withBackdrop = rois.filter((p) => p.roiBackdrop);
  const pool = withBackdrop.length > 0 ? withBackdrop : rois;
  const frames: RoiFrame[] = [];
  for (const p of pool) {
    const key = frameKeyOf(p);
    let frame = frames.find((f) => f.key === key);
    if (!frame) {
      frame = { key, label: frameLabel(p, eff), params: [], files: backdropFiles(p, eff) };
      frames.push(frame);
    }
    frame.params.push(p);
  }
  return frames;
}

function frameLabel(p: Param, eff: Record<string, unknown>): string {
  const b = p.roiBackdrop;
  if (!b) return "";
  const base = b.label ?? p.group ?? "";
  const extra = b.labelParam ? String(eff[b.labelParam] ?? "") : "";
  return extra ? (base ? `${base} · ${extra}` : extra) : base;
}

function backdropFiles(p: Param, eff: Record<string, unknown>): string[] {
  const b = p.roiBackdrop;
  if (!b) return [];
  const dir = String(eff[b.dir] ?? "");
  if (!dir) return [];
  return (b.files ?? [])
    .map((f) => String(eff[f] ?? ""))
    .filter(Boolean)
    .map((f) => joinPath(dir, f));
}

/** 当前该画哪一组：选过且还在（槽还启用着）就是它，否则第一组。 */
export function pickFrame(frames: readonly RoiFrame[], selected: string | undefined): RoiFrame | null {
  return frames.find((f) => f.key === selected) ?? frames[0] ?? null;
}

/** 一个参数组（Inspector 里的一节）对应哪一组框：组里第一个带底图的 roi 参数的底图。
 *  不看可见性 —— 没启用的槽那一节照样认得出是「模板 3」，展开它才能去勾 Enabled。 */
export function frameKeyOfGroup(params: readonly Param[]): string | null {
  const p = params.find((q) => isRoi(q) && q.roiBackdrop);
  return p ? frameKeyOf(p) : null;
}

/** 「把当前这组框复制到其它组」要写的参数：按组内顺序一一对应（locate_template 每个槽都是
 *  datum、target、seamLeft、seamRight 的顺序）；框数对不上的组跳过。值取当前组的有效值。 */
export function copyFrameWrites(
  op: OperatorDesc,
  node: GraphNode,
  from: RoiFrame,
  frames: readonly RoiFrame[],
): { param: string; value: number[] }[] {
  const eff = effectiveParams(op, node);
  const writes: { param: string; value: number[] }[] = [];
  for (const to of frames) {
    if (to.key === from.key || to.params.length !== from.params.length) continue;
    to.params.forEach((p, i) => {
      const raw = eff[from.params[i]!.name];
      const value = Array.isArray(raw) && raw.length === 4 ? raw.map(Number) : [0, 0, 0, 0];
      writes.push({ param: p.name, value });
    });
  }
  return writes;
}

// ---------------------------------------------------------------- 标签避让（L21）

export interface LabelBox {
  /** 框在层里的像素矩形。 */
  left: number;
  top: number;
  width: number;
  height: number;
  /** 标签自己的尺寸。 */
  labelWidth: number;
  labelHeight: number;
}

/** 标签相对框左上角的偏移。 */
export interface LabelSpot {
  dx: number;
  dy: number;
  /** above = 框上方（默认）、inside = 框内左上、stack = 上方再错开几行、below = 框下方。 */
  where: "above" | "inside" | "stack" | "below";
}

interface Rect {
  x0: number;
  y0: number;
  x1: number;
  y1: number;
}

function overlaps(a: Rect, b: Rect): boolean {
  return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
}

/** 给每个框的标签挑一个不压住别的标签的位置：从左到右逐个放，依次试「框上方」「框内左上」
 *  「上方错开一到三行」「框下方」，取第一个与已放好的标签都不相交的；都不行就退回上方。
 *  相邻的框（Seam Right 挨着 Target）就这样一个在上、一个在内侧，或者上下错开。 */
export function placeLabels(boxes: readonly LabelBox[]): LabelSpot[] {
  const spots: LabelSpot[] = boxes.map(() => ({ dx: -1, dy: 0, where: "above" }));
  const order = boxes.map((_, i) => i).sort((a, b) => boxes[a]!.left - boxes[b]!.left || a - b);
  const placed: Rect[] = [];
  for (const i of order) {
    const b = boxes[i]!;
    const w = b.labelWidth;
    const h = b.labelHeight;
    const tries: LabelSpot[] = [
      { dx: -1, dy: -h, where: "above" },
      { dx: 1, dy: 1, where: "inside" },
      { dx: -1, dy: -h - (h + 1), where: "stack" },
      { dx: -1, dy: -h - 2 * (h + 1), where: "stack" },
      { dx: -1, dy: -h - 3 * (h + 1), where: "stack" },
      { dx: -1, dy: b.height, where: "below" },
    ];
    const rectOf = (s: LabelSpot): Rect => ({
      x0: b.left + s.dx,
      y0: b.top + s.dy,
      x1: b.left + s.dx + w,
      y1: b.top + s.dy + h,
    });
    const fits = (s: LabelSpot) => {
      // 框内放不下（框比标签还窄/矮）就不算这一格
      if (s.where === "inside" && (w + 2 > b.width || h + 2 > b.height)) return false;
      const r = rectOf(s);
      return placed.every((q) => !overlaps(r, q));
    };
    const spot = tries.find(fits) ?? tries[0]!;
    spots[i] = spot;
    placed.push(rectOf(spot));
  }
  return spots;
}
