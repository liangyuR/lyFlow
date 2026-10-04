// 检查器的「参数说明」（默认收起，开合记住）：每个参数下面写出它的说明、默认值、范围与单位。
// 以前说明只有悬停在名字上才看得到，默认值要去看 ↺ 的悬停提示，范围要把滑块拖到头才知道。

import { num } from "./format";
import type { Param } from "../types/manifest";

/** 开着还是收着，所有节点共用一份（lib/prefs 的 "1" / "0"）。默认收起：检查器以调参为主，说明是要时才看。 */
export const PARAM_DOCS_KEY = "lyflow.inspector.paramDocs";

function valueText(param: Param, v: unknown): string {
  if (param.options && (typeof v === "string" || typeof v === "number")) {
    const opt = param.options.find((o) => o.value === v);
    if (opt) return opt.label || String(opt.value);
  }
  if (typeof v === "number") return num(v);
  if (typeof v === "boolean") return v ? "开" : "关";
  if (typeof v === "string") return v === "" ? "空" : v.length > 32 ? `${v.slice(0, 31)}…` : v;
  if (Array.isArray(v)) return `[${v.map((x) => (typeof x === "number" ? num(x) : String(x))).join(", ")}]`;
  if (v === undefined || v === null) return "无";
  const text = JSON.stringify(v);
  return text.length > 32 ? `${text.slice(0, 31)}…` : text;
}

const numeric = (v: unknown): boolean => typeof v === "number" || (Array.isArray(v) && v.length > 0 && v.every((x) => typeof x === "number"));

function rangeText(lo: number | undefined, hi: number | undefined, unit: string): string | null {
  if (lo !== undefined && hi !== undefined) return `${num(lo)} – ${num(hi)}${unit}`;
  if (lo !== undefined) return `≥ ${num(lo)}${unit}`;
  if (hi !== undefined) return `≤ ${num(hi)}${unit}`;
  return null;
}

/** 说明下面那一行：「默认 0.02 mm · 范围 0 – 1 mm · 常用 0 – 0.5 mm」；枚举再写「可选 A / B」。
 *  单位只跟在数（与数组）后面；没有范围的数照样写单位。 */
export function paramFacts(param: Param): string {
  const unit = param.unit ? ` ${param.unit}` : "";
  const parts = [`默认 ${valueText(param, param.default)}${numeric(param.default) ? unit : ""}`];
  const hard = rangeText(param.min, param.max, unit);
  if (hard) parts.push(`范围 ${hard}`);
  const soft = rangeText(param.softMin, param.softMax, unit);
  if (soft && soft !== hard) parts.push(`常用 ${soft}`);
  if (param.unit && !numeric(param.default) && !hard && !soft) parts.push(`单位 ${param.unit}`);
  if (param.options && param.options.length > 0) {
    parts.push(`可选 ${param.options.map((o) => o.label || String(o.value)).join(" / ")}`);
  }
  return parts.join(" · ");
}
