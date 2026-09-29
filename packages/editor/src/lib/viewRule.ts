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

/** 主预览显示什么：点云场景（含底图与 2D 叠画），还是输出值的表格。 */
export type ViewerContent = "cloud" | "value";

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

/** 节点的全部输出类型 → 主预览的内容。有一个可画的端口就是点云场景；
 *  全是 Record / Measurement / Plane / Tensor / Image 这类只有值的，才换成表格
 *  （主预览的图像模式在 docs/image-plan.md 阶段 3，之前 Image 在主预览里只有尺寸与统计量）。 */
export function viewerContentFor(
  types: readonly (string | null)[],
  bundles?: readonly BundleDesc[],
): ViewerContent {
  if (types.length === 0) return "cloud";
  return types.some((t) => drawable(t, bundles)) ? "cloud" : "value";
}

/** 对比模式两栏合起来的内容（compare-plan §1.6）：一侧可画就两栏都是点云场景 —— 只有值的那一侧
 *  显示「无点云输出」，它的值照样进差异表；两侧都只有值才换成两张并排的值表格。 */
export function compareContentFor(a: ViewerContent, b: ViewerContent): ViewerContent {
  return a === "cloud" || b === "cloud" ? "cloud" : "value";
}
