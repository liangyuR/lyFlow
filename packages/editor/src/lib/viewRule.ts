// 按输出类型选视图（docs/edge-peek-plan.md §4.3）。连线查看器按端口选、主预览按节点选，
// 规则只在这一处：两边各写一份，迟早会出现「连线上看是表格、选中节点却是空白」。
// 纯函数，不碰 store —— 单测直接测它（test/view-rule.test.mjs）。

import type { PeekView } from "../store/peek";
import { bundleKindOf, type BundleDesc } from "../types/manifest";

export function defaultViewFor(type: string | null): PeekView {
  if (type && bundleKindOf(type)) return "fields";
  switch (type) {
    case "PointCloud":
      return "cloud3d";
    case "Box2D":
    case "Line2D":
    case "Circle2D":
    case "Point2D":
      return "cloud2d";
    case "Tensor":
      return "tensor";
    case "Image":
      return "image";
    case "Indices":
      return "indices";
    default:
      return "value";
  }
}

const VIEWS_POINT_CLOUD: PeekView[] = ["cloud3d", "cloud2d", "value"];
const VIEWS_SHAPE_2D: PeekView[] = ["cloud2d", "value"];
const VIEWS_TENSOR: PeekView[] = ["tensor", "value"];
const VIEWS_IMAGE: PeekView[] = ["image", "value"];
const VIEWS_INDICES: PeekView[] = ["indices"];
const VIEWS_VALUE: PeekView[] = ["value"];
const VIEWS_BUNDLE: PeekView[] = ["fields", "value"];

export function viewsFor(type: string | null): PeekView[] {
  if (type && bundleKindOf(type)) return VIEWS_BUNDLE;
  switch (type) {
    case "PointCloud":
      return VIEWS_POINT_CLOUD;
    case "Box2D":
    case "Line2D":
    case "Circle2D":
    case "Point2D":
      return VIEWS_SHAPE_2D;
    case "Tensor":
      return VIEWS_TENSOR;
    case "Image":
      return VIEWS_IMAGE;
    case "Indices":
      return VIEWS_INDICES;
    default:
      return VIEWS_VALUE;
  }
}

/** 主预览显示什么：点云场景（含底图与 2D 叠画）、图像（含像素几何叠画与像素框，docs/image-plan.md 阶段 3），
 *  还是输出值的表格。 */
export type ViewerContent = "cloud" | "image" | "value";

/** 这个类型在点云场景里有东西可画：点云本身、叠在底图上的 2D 几何，或者指向一片云的
 *  Indices（底图从上游借，见 lib/basecloud）。类型还不知道（`Any` 没跑过）时也算，
 *  保持引入这条规则之前的行为。 */
function drawable(type: string | null, bundles: readonly BundleDesc[] | undefined): boolean {
  if (type === null || type === "Any") return true;
  const view = defaultViewFor(type);
  if (view === "fields") {
    const kind = bundleKindOf(type);
    const decl = bundles?.find((b) => b.kind === kind);
    return !decl || decl.fields.some((f) => drawable(f.type, bundles));
  }
  return view === "cloud3d" || view === "cloud2d" || view === "indices";
}

/** 输出里有点云本身（或指向点云的 Indices）—— 不算 2D 几何，那要看它属于哪个域。 */
function hasCloud(types: readonly (string | null)[]): boolean {
  return types.some((t) => t === "PointCloud" || t === "Indices");
}

/** 节点的全部输出类型 → 主预览的内容。
 *  - 有点云（或 Indices）→ 点云场景；
 *  - 否则只要进出有一个 Image 就是图像域：输出的 2D 几何是像素坐标，叠在图上（找圆、区域统计的框），
 *    没有输出图像的节点显示它输入的那张图；
 *  - 否则有一个可画的端口（2D 几何叠在上游借来的底图上）→ 点云场景；
 *  - 全是 Record / Measurement / Plane / Tensor 这类只有值的，才换成表格。
 *  `inputTypes` 缺省时按老规则（不看输入）。 */
export function viewerContentFor(
  types: readonly (string | null)[],
  bundles?: readonly BundleDesc[],
  inputTypes: readonly (string | null)[] = [],
): ViewerContent {
  if (types.length === 0 && inputTypes.length === 0) return "cloud";
  if (hasCloud(types)) return "cloud";
  if (types.includes("Image") || inputTypes.includes("Image")) return "image";
  if (types.length === 0) return "cloud";
  return types.some((t) => drawable(t, bundles)) ? "cloud" : "value";
}

/** 对比模式两栏合起来的内容（compare-plan §1.6）：一侧可画就两栏都是点云场景 —— 只有值的那一侧
 *  显示「无点云输出」，它的值照样进差异表；两侧都只有值才换成两张并排的值表格。
 *  图像在对比里按值算（尺寸与逐通道统计进差异表）：两张图并排是以后的事（image-plan 阶段 3 不做）。 */
export function compareContentFor(a: ViewerContent, b: ViewerContent): ViewerContent {
  return a === "cloud" || b === "cloud" ? "cloud" : "value";
}
