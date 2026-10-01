// 一级图像按段取齐（docs/image-plan.md §5.4）：桥接层按 16 MB 收行数，照帧头的 rowCount 接着要。
// 连线查看器与主预览共用（ImageCanvas）。取数函数由调用方给 —— 单测用假的，不必真有 16 MB 的图。
import { decodeImage, type ImagePayload } from "../types/execution";

export interface FetchedLevel {
  width: number;
  height: number;
  channels: number;
  /** 实际给出的级别（可能与请求的不同：要得太大时收到 1 像素那级，HTTP 桩只给 0）。 */
  level: number;
  pixels: Uint8Array | Uint16Array | Float32Array;
}

/** `Transport.getOutputImage` 的形状：第 level 级、从 row 起 rows 行（0 = 到底），LYIM 载荷。 */
export type GetImage = (
  runId: string,
  nodeId: string,
  port: string,
  level: number,
  row: number,
  rows: number,
) => Promise<ArrayBuffer>;

/** 取齐之前作废了（换了图、换了级别、组件卸掉）就别再要后面几段：一段 16 MB，二十兆像素的 RGB 原图要四段。 */
export class FetchAbandoned extends Error {
  constructor() {
    super("取图已作废");
  }
}

export async function fetchImageLevel(
  get: GetImage,
  runId: string,
  nodeId: string,
  port: string,
  level: number,
  abandoned: () => boolean = () => false,
): Promise<FetchedLevel> {
  let first: ImagePayload | null = null;
  let pixels: Uint8Array | Uint16Array | Float32Array | null = null;
  let row = 0;
  for (;;) {
    const part = decodeImage(await get(runId, nodeId, port, level, row, 0));
    if (first === null) {
      first = part;
      const n = part.width * part.height * part.channels;
      pixels =
        part.depth === 1 ? new Uint8Array(n) : part.depth === 2 ? new Uint16Array(n) : new Float32Array(n);
    }
    if (part.rowCount === 0) break;
    pixels!.set(part.pixels, part.rowOffset * part.width * part.channels);
    row = part.rowOffset + part.rowCount;
    if (row >= part.height) break;
    if (abandoned()) throw new FetchAbandoned();
  }
  if (first === null || pixels === null) throw new Error("没取到图像");
  return { width: first.width, height: first.height, channels: first.channels, level: first.level, pixels };
}
