// 图像载荷（LYIM，docs/http-transport.md「图像」）的解码，与给 agent 看的 PNG（docs/image-plan.md §5.5）。
// PNG 用 Node 自带的 zlib 手写，不加依赖：只要 8 位的灰度 / RGB / RGBA 三种，过滤器一律 None。
import zlib from "node:zlib";

export const IMAGE_MAGIC = 0x4d49594c;
export const IMAGE_HEADER_BYTES = 48;

export interface ImageSlice {
  width: number;
  height: number;
  channels: number;
  /** 每通道字节数：1 = u8，2 = u16，4 = f32。 */
  depth: 1 | 2 | 4;
  level: number;
  fullWidth: number;
  fullHeight: number;
  rowOffset: number;
  rowCount: number;
  /** rowCount · width · channels 个通道值，转成 number。 */
  values: Float64Array;
}

export function decodeImage(buffer: ArrayBuffer): ImageSlice {
  if (buffer.byteLength < IMAGE_HEADER_BYTES) {
    throw new Error(`图像载荷太短（${buffer.byteLength} 字节），多半不是图像数据`);
  }
  const view = new DataView(buffer);
  const u = (i: number) => view.getUint32(i * 4, true);
  if (u(0) !== IMAGE_MAGIC) {
    throw new Error(`图像载荷的 magic 不对（0x${u(0).toString(16)}），多半不是图像数据`);
  }
  const depth = u(4);
  if (depth !== 1 && depth !== 2 && depth !== 4) throw new Error(`图像载荷的位深不认识（${depth}）`);
  const width = u(1);
  const channels = u(3);
  const rowCount = u(9);
  const rowBytes = u(10);
  const need = IMAGE_HEADER_BYTES + rowCount * rowBytes;
  if (buffer.byteLength < need) {
    throw new Error(`图像载荷被截断：要 ${need} 字节，只有 ${buffer.byteLength}`);
  }
  const n = rowCount * width * channels;
  const values = new Float64Array(n);
  for (let i = 0; i < n; i += 1) {
    const at = IMAGE_HEADER_BYTES + i * depth;
    values[i] =
      depth === 1 ? view.getUint8(at) : depth === 2 ? view.getUint16(at, true) : view.getFloat32(at, true);
  }
  return {
    width,
    height: u(2),
    channels,
    depth,
    level: u(5),
    fullWidth: u(6),
    fullHeight: u(7),
    rowOffset: u(8),
    rowCount,
    values,
  };
}

/** 按段取齐一级图像：服务端按 16 MB 收行数，照帧头的 rowCount 接着要；HTTP 桩只给 level 0（帧头照实写），
 *  后续几段按帧头里实际给出的级别要。`get(level, row)` 取从 row 起到底的那一段。 */
export async function fetchImage(
  get: (level: number, row: number) => Promise<ArrayBuffer>,
  level: number,
): Promise<{ first: ImageSlice; values: Float64Array }> {
  const first = decodeImage(await get(level, 0));
  const values = new Float64Array(first.width * first.height * first.channels);
  let part = first;
  for (;;) {
    values.set(part.values, part.rowOffset * part.width * part.channels);
    const next = part.rowOffset + part.rowCount;
    if (part.rowCount === 0 || next >= part.height) break;
    part = decodeImage(await get(first.level, next));
  }
  return { first, values };
}

/** 长边不超过 maxEdge 的最小级别（与 core 的缩小同一个取整：向上取整）。 */
export function levelToFit(width: number, height: number, maxEdge: number): number {
  let level = 0;
  while (Math.max(Math.ceil(width / 2 ** level), Math.ceil(height / 2 ** level)) > maxEdge && level < 30) {
    level += 1;
  }
  return level;
}

export interface Picture {
  width: number;
  height: number;
  channels: number;
  /** 0..255。 */
  bytes: Uint8Array;
  /** 转 8 位时用的范围：u8 是 [0, 255]，u16 / f32 按这张图的有限值拉伸。 */
  range: [number, number];
}

/** 一整张图（各段拼好的值）转成 8 位；超过 maxEdge 时再按最近邻缩（HTTP 桩只给 level 0）。 */
export function toPicture(
  width: number,
  height: number,
  channels: number,
  depth: 1 | 2 | 4,
  values: Float64Array,
  maxEdge: number,
): Picture {
  let lo = 0;
  let hi = 255;
  if (depth !== 1) {
    lo = Infinity;
    hi = -Infinity;
    for (const v of values) {
      if (!Number.isFinite(v)) continue;
      if (v < lo) lo = v;
      if (v > hi) hi = v;
    }
    if (!(hi > lo)) {
      lo = 0;
      hi = 1;
    }
  }
  const step = Math.max(1, Math.ceil(Math.max(width, height) / maxEdge));
  const w = Math.ceil(width / step);
  const h = Math.ceil(height / step);
  const bytes = new Uint8Array(w * h * channels);
  const span = hi - lo;
  for (let y = 0; y < h; y += 1) {
    for (let x = 0; x < w; x += 1) {
      const src = ((y * step) * width + x * step) * channels;
      const dst = (y * w + x) * channels;
      for (let c = 0; c < channels; c += 1) {
        const v = values[src + c]!;
        bytes[dst + c] = Number.isFinite(v) ? Math.round(Math.min(1, Math.max(0, (v - lo) / span)) * 255) : 0;
      }
    }
  }
  return { width: w, height: h, channels, bytes, range: [lo, hi] };
}

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n += 1) {
    let c = n;
    for (let k = 0; k < 8; k += 1) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();

function crc32(bytes: Uint8Array): number {
  let c = 0xffffffff;
  for (const b of bytes) c = CRC_TABLE[(c ^ b) & 0xff]! ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

function chunk(type: string, data: Uint8Array): Buffer {
  const out = Buffer.alloc(12 + data.length);
  out.writeUInt32BE(data.length, 0);
  out.write(type, 4, "ascii");
  Buffer.from(data).copy(out, 8);
  out.writeUInt32BE(crc32(out.subarray(4, 8 + data.length)), 8 + data.length);
  return out;
}

/** 8 位灰度 / RGB / RGBA 的 PNG。 */
export function encodePng(p: Picture): Buffer {
  const colorType = p.channels === 1 ? 0 : p.channels === 3 ? 2 : p.channels === 4 ? 6 : -1;
  if (colorType < 0) throw new Error(`PNG 只画 1 / 3 / 4 通道，这张是 ${p.channels}`);
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(p.width, 0);
  ihdr.writeUInt32BE(p.height, 4);
  ihdr[8] = 8; // 位深
  ihdr[9] = colorType;
  const stride = p.width * p.channels;
  const raw = Buffer.alloc((stride + 1) * p.height);
  for (let y = 0; y < p.height; y += 1) {
    raw[y * (stride + 1)] = 0; // 过滤器 None
    Buffer.from(p.bytes.buffer, p.bytes.byteOffset + y * stride, stride).copy(raw, y * (stride + 1) + 1);
  }
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk("IHDR", ihdr),
    chunk("IDAT", zlib.deflateSync(raw)),
    chunk("IEND", new Uint8Array(0)),
  ]);
}
