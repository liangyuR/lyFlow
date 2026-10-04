// 检查器的「参数说明」（默认收起，开合记住）：每个参数下面写出它的说明、默认值、范围与单位。
// 以前说明只有悬停在名字上才看得到，默认值要去看 ↺ 的悬停提示，范围要把滑块拖到头才知道。

import { num } from "./format";
import { formatValue } from "./recipes";
import type { Param } from "../types/manifest";

/** 开着还是收着，所有节点共用一份（lib/prefs 的 "1" / "0"）。默认收起：检查器以调参为主，说明是要时才看。 */
export const PARAM_DOCS_KEY = "lyflow.inspector.paramDocs";

/** 默认值怎么写：与配方单元格同一套（transform 写成平移 / 旋转、curve 写点数、颜色写 #hex），
 *  flags 写开着的那几项，空字符串写「空」。 */
function valueText(param: Param, v: unknown): string {
  if (v === "") return "空";
  if (param.type === "flags" && typeof v === "number" && param.options) {
    const on = param.options.filter((o) => typeof o.value === "number" && (v & o.value) !== 0).map((o) => o.label || String(o.value));
    return on.length > 0 ? on.join(" + ") : "无";
  }
  return formatValue(v, param);
}

/** 单位跟在默认值后面的那几种：数与数的向量（transform 的 unit 是平移的单位、颜色没有单位，另写）。 */
function unitFollowsValue(param: Param): boolean {
  if (param.type === "transform" || param.type === "color" || param.type === "curve") return false;
  const v = param.default;
  return typeof v === "number" || (Array.isArray(v) && v.length > 0 && v.every((x) => typeof x === "number"));
}

function rangeText(lo: number | undefined, hi: number | undefined, unit: string): string | null {
  if (lo !== undefined && hi !== undefined) return `${num(lo)} – ${num(hi)}${unit}`;
  if (lo !== undefined) return `≥ ${num(lo)}${unit}`;
  if (hi !== undefined) return `≤ ${num(hi)}${unit}`;
  return null;
}

/** 说明下面那一行：「默认 0.02 mm · 范围 0 – 1 mm · 常用 0 – 0.5 mm」；枚举再写「可选 A / B」。
 *  curve 的上下限管的是 y（写「y 范围」「画布 y」）；transform 的单位是平移的。 */
export function paramFacts(param: Param): string {
  const unit = param.unit ? ` ${param.unit}` : "";
  const follows = unitFollowsValue(param);
  const parts = [`默认 ${valueText(param, param.default)}${follows ? unit : ""}`];
  const curve = param.type === "curve";
  const hard = rangeText(param.min, param.max, follows ? unit : "");
  if (hard) parts.push(`${curve ? "y 范围" : "范围"} ${hard}`);
  const soft = rangeText(param.softMin, param.softMax, follows ? unit : "");
  if (soft && soft !== hard) parts.push(`${curve ? "画布 y" : "常用"} ${soft}`);
  if (param.unit && !follows) parts.push(param.type === "transform" ? `平移单位 ${param.unit}` : `单位 ${param.unit}`);
  if (param.options && param.options.length > 0) {
    parts.push(`可选 ${param.options.map((o) => o.label || String(o.value)).join(" / ")}`);
  }
  return parts.join(" · ");
}
