// 输出值怎么写成字：检查器的「输出」、连线查看器的字段表、节点底栏的读数、运行收尾共用这一处，
// 同一个值在几处写法不一样，读的人会以为是两个数。

import { num } from "./format";
import type { OutputStat, OutputValue, SummaryOutput } from "../types/execution";

/** 节点这次的输出（OutputStat）与运行收尾里的图级输出（SummaryOutput）都有这两样。 */
type ValueLike = { elementCount?: number | undefined; value?: OutputValue | undefined };

function pair(v: [number, number] | number | null | undefined): string {
  return Array.isArray(v) ? `(${num(v[0])}, ${num(v[1])})` : "—";
}

/** 非点云输出的一行文本。类型未知时退回类型名，永远不抛。 */
export function formatOutputValue(o: ValueLike): string {
  const v: OutputValue | undefined = o.value;
  if (!v) return `${o.elementCount ?? 0} 个元素`;
  // 图像算子的几何是像素坐标（docs/image-plan.md Q2）：值上写明，免得当成米读
  const px = v.unit === "px" && v.kind !== "Measurement" ? " px" : "";
  switch (v.kind) {
    case "Measurement":
      if (v.value === null || v.value === undefined) return v.message || "未测出";
      return `${num(v.value)} ${v.unit ?? ""}`.trim();
    case "Box2D":
      return `${pair(v.min)} → ${pair(v.max)}${px}`;
    case "Line2D":
      return v.hasSegment
        ? `${pair(v.start)} → ${pair(v.end)}${px}`
        : `过 ${pair(v.point)} 方向 ${pair(v.dir)}${px}`;
    case "Circle2D":
      return `圆心 ${pair(v.center)} 半径 ${num(v.radius)}${px}`;
    case "Point2D":
      return `${pair(v.p)}${px}`;
    case "Record":
      return `${v.type ?? ""} ${JSON.stringify(v.data ?? {})}`.trim();
    case "Plane":
      return `n=(${(v.normal ?? []).map(num).join(", ")}) d=${num(v.d)}`;
    case "Tensor":
      return `[${(v.shape ?? []).join(", ")}] 均值 ${num(typeof v.mean === "number" ? v.mean : undefined)}`;
    case "Image": {
      const means = Array.isArray(v.mean) ? v.mean.map((m) => num(m ?? undefined)).join(", ") : "—";
      return `${v.width ?? "?"}×${v.height ?? "?"}×${v.channels ?? "?"} ${v.depth ?? ""} · 均值 (${means})`;
    }
    default:
      return `${o.elementCount ?? 0} 个元素`;
  }
}

/** 判定分三档上色：不合格（fail / high / low）、接近边界（margin）、合格（ok）。空串、没判定、不认识的不上色。 */
export type VerdictTone = "ng" | "margin" | "ok";

export function verdictTone(verdict: string | null | undefined): VerdictTone | null {
  switch (verdict) {
    case "ok":
      return "ok";
    case "margin":
      return "margin";
    case "fail":
    case "high":
    case "low":
      return "ng";
    default:
      return null;
  }
}

/** 节点底栏上的一个读数：量测输出（Measurement）的值与判定。 */
export interface Reading {
  port: string;
  /** 底栏上写的：四位有效数字 + 单位；没测出来写「未测出」。 */
  text: string;
  verdict: string | null;
  tone: VerdictTone | null;
  /** 悬停看全文：端口、六位有效数字、判定、标称与上下限，没测出来时的原因。 */
  title: string;
}

/** 底栏只有一行：一个节点最多写这么多个读数，多的写「+N」（全部在检查器的「输出」里）。 */
export const MAX_NODE_READINGS = 2;

/** 这次运行的量测读数，按输出声明的顺序。以前节点底栏只写元素数（量测节点永远是「1」），
 *  读数要一个个点开节点在检查器里看 —— 而读 gap / flush 是每次运行的目的。 */
export function readingsOf(outputs: readonly OutputStat[] | undefined): Reading[] {
  const out: Reading[] = [];
  for (const o of outputs ?? []) {
    const v = o.value;
    if (v?.kind !== "Measurement") continue;
    const measured = typeof v.value === "number" && Number.isFinite(v.value);
    const text = measured ? `${Number((v.value as number).toPrecision(4))}${v.unit ? ` ${v.unit}` : ""}` : "未测出";
    const verdict = v.verdict || null;
    const limits = [
      v.nominal !== undefined ? `标称 ${num(v.nominal)}` : null,
      v.lower !== undefined ? `下限 ${num(v.lower)}` : null,
      v.upper !== undefined ? `上限 ${num(v.upper)}` : null,
    ].filter(Boolean);
    const title =
      `${o.port} = ${formatOutputValue(o)}` +
      (verdict ? `（${verdict}）` : "") +
      (limits.length > 0 ? `；${limits.join("，")}` : "") +
      (measured && v.message ? `；${v.message}` : "");
    out.push({ port: o.port, text, verdict, tone: verdictTone(verdict), title });
  }
  return out;
}

/** 运行收尾里图级输出的顺序：有问题的在上（崩了的、判不合格的），再是接近边界的，其余照原样。稳定排序。 */
export function sortSummaryOutputs<T extends Pick<SummaryOutput, "state" | "value">>(
  entries: readonly (readonly [string, T])[],
): (readonly [string, T])[] {
  const rank = (o: T) => {
    if (o.state === "failed") return 0;
    const tone = o.value?.kind === "Measurement" ? verdictTone(o.value.verdict) : null;
    return tone === "ng" ? 0 : tone === "margin" ? 1 : 2;
  };
  return entries
    .map((e, i) => ({ e, i, r: rank(e[1]) }))
    .sort((a, b) => a.r - b.r || a.i - b.i)
    .map((x) => x.e);
}

/** 一个节点这次的量测判定里最差的那个（不合格 > 接近边界 > 合格）；没有判定的 null。查找节点的 is:ng / is:margin 用。 */
export function worstTone(outputs: readonly OutputStat[] | undefined): VerdictTone | null {
  let worst: VerdictTone | null = null;
  for (const o of outputs ?? []) {
    if (o.value?.kind !== "Measurement") continue;
    const tone = verdictTone(o.value.verdict);
    if (tone === "ng") return "ng";
    if (tone === "margin" || (tone === "ok" && worst === null)) worst = tone;
  }
  return worst;
}

/** 这次运行的判定一共几个：不合格、接近边界、合格、没测出来（值不是数）。工具栏上的「NG 2 · 边界 1 · ok 12」。 */
export interface VerdictTally {
  ng: number;
  margin: number;
  ok: number;
  unmeasured: number;
}

export function verdictTally(readings: readonly { value: number | null; verdict: string | null }[]): VerdictTally {
  const out: VerdictTally = { ng: 0, margin: 0, ok: 0, unmeasured: 0 };
  for (const r of readings) {
    if (r.value === null && !r.verdict) {
      out.unmeasured += 1;
      continue;
    }
    const tone = verdictTone(r.verdict);
    if (tone) out[tone] += 1;
  }
  return out;
}

