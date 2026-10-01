import assert from "node:assert/strict";
import test from "node:test";

import {
  CLOUD_HAS_INTENSITY,
  CLOUD_HAS_RGB,
  CLOUD_MAGIC,
  INDICES_MAGIC,
  TENSOR_MAGIC,
  decodeCloud,
  decodeIndices,
  decodeTensor,
  summarizeCloud,
} from "../src/cloud.js";
import { IMAGE_MAGIC, decodeImage, encodePng, fetchImage, levelToFit, toPicture } from "../src/image.js";

function encode(
  xyz: number[],
  options?: { intensity?: number[]; rgb?: number[]; totalPoints?: number; bounds?: number[] },
): ArrayBuffer {
  const n = xyz.length / 3;
  const intensity = options?.intensity ?? null;
  const rgb = options?.rgb ?? null;
  const flags = (intensity ? CLOUD_HAS_INTENSITY : 0) | (rgb ? CLOUD_HAS_RGB : 0);
  const bounds = options?.bounds ?? [
    Math.min(...xyz.filter((_, i) => i % 3 === 0)),
    Math.min(...xyz.filter((_, i) => i % 3 === 1)),
    Math.min(...xyz.filter((_, i) => i % 3 === 2)),
    Math.max(...xyz.filter((_, i) => i % 3 === 0)),
    Math.max(...xyz.filter((_, i) => i % 3 === 1)),
    Math.max(...xyz.filter((_, i) => i % 3 === 2)),
  ];
  // rgb 块放最后、补齐到 4 字节（与 bridge 的 encode_cloud 一致）
  const buffer = new ArrayBuffer(40 + n * 12 + (intensity ? n * 4 : 0) + (rgb ? (n * 3 + 3) & ~3 : 0));
  const view = new DataView(buffer);
  view.setUint32(0, CLOUD_MAGIC, true);
  view.setUint32(4, n, true);
  view.setUint32(8, options?.totalPoints ?? n, true);
  view.setUint32(12, flags, true);
  for (let i = 0; i < 6; i += 1) view.setFloat32(16 + i * 4, bounds[i] as number, true);
  let off = 40;
  for (const v of xyz) {
    view.setFloat32(off, v, true);
    off += 4;
  }
  for (const v of intensity ?? []) {
    view.setFloat32(off, v, true);
    off += 4;
  }
  for (const v of rgb ?? []) {
    view.setUint8(off, v);
    off += 1;
  }
  return buffer;
}

test("rgb 块（C ABI v12）排在强度之后：每点三个 0..255，统计进 r/g/b 通道、head 带 rgb", () => {
  const summary = summarizeCloud(
    decodeCloud(encode([0, 0, 0, 1, 1, 1], { intensity: [0.5, 1], rgb: [255, 0, 10, 0, 128, 20] })),
    2,
  );
  assert.deepEqual(
    {
      r: summary.channels["r"],
      intensity: summary.channels["intensity"]?.max,
      head: summary.head.map((p) => p.rgb),
    },
    { r: { min: 0, max: 255, mean: 127.5 }, intensity: 1, head: [[255, 0, 10], [0, 128, 20]] },
  );
});

test("解出点云并算出 bbox 与每通道统计", () => {
  const buffer = encode([0, 0, 0, 1, 2, 3, 2, 4, 6, 3, 6, 9], {
    intensity: [0.25, 0.5, 0.75, 1],
  });
  const summary = summarizeCloud(decodeCloud(buffer), 2);

  assert.equal(summary.pointCount, 4);
  assert.equal(summary.totalPoints, 4);
  assert.deepEqual(summary.bbox.min, [0, 0, 0]);
  assert.deepEqual(summary.bbox.max, [3, 6, 9]);
  assert.deepEqual(summary.channels["x"], { min: 0, max: 3, mean: 1.5 });
  assert.deepEqual(summary.channels["y"], { min: 0, max: 6, mean: 3 });
  assert.deepEqual(summary.channels["z"], { min: 0, max: 9, mean: 4.5 });
  assert.deepEqual(summary.channels["intensity"], { min: 0.25, max: 1, mean: 0.625 });
  assert.equal(summary.head.length, 2);
  assert.deepEqual(summary.head[0], { x: 0, y: 0, z: 0, intensity: 0.25 });
  assert.deepEqual(summary.head[1], { x: 1, y: 2, z: 3, intensity: 0.5 });
});

test("bbox 用的是头部的全量包围盒，不是抽稀之后这几个点", () => {
  const buffer = encode([1, 1, 1, 2, 2, 2], {
    totalPoints: 1000,
    bounds: [-5, -5, -5, 5, 5, 5],
  });
  const summary = summarizeCloud(decodeCloud(buffer), 8);
  assert.equal(summary.pointCount, 2);
  assert.equal(summary.totalPoints, 1000);
  assert.deepEqual(summary.bbox.min, [-5, -5, -5]);
  assert.deepEqual(summary.bbox.max, [5, 5, 5]);
  assert.equal(summary.head.length, 2);
  assert.equal(summary.channels["intensity"], undefined);
});

test("空点云不炸，统计是 null", () => {
  const summary = summarizeCloud(decodeCloud(encode([], { bounds: [0, 0, 0, 0, 0, 0] })), 8);
  assert.equal(summary.pointCount, 0);
  assert.deepEqual(summary.channels["x"], { min: null, max: null, mean: null });
  assert.deepEqual(summary.head, []);
});

test("magic 不对或太短的载荷直接报错", () => {
  assert.throws(() => decodeCloud(new ArrayBuffer(8)), /太短/);
  const buffer = encode([0, 0, 0]);
  new DataView(buffer).setUint32(0, 0x12345678, true);
  assert.throws(() => decodeCloud(buffer), /magic/);
  // 张量 / 下标的解码器同一口径：点云载荷塞给它们也认得出不是自己
  assert.throws(() => decodeTensor(new ArrayBuffer(8)), /太短/);
  assert.throws(() => decodeTensor(encode([0, 0, 0])), /magic/);
  assert.throws(() => decodeIndices(new ArrayBuffer(8)), /太短/);
  assert.throws(() => decodeIndices(encode([0, 0, 0])), /magic/);
});

/** 按 docs/http-transport.md 的布局造一片张量：完整形状 + 从 offset 起的 values。 */
function encodeTensor(shape: number[], offset: number, total: number, values: number[]): ArrayBuffer {
  const buffer = new ArrayBuffer(32 + shape.length * 8 + values.length * 4);
  const view = new DataView(buffer);
  view.setUint32(0, TENSOR_MAGIC, true);
  view.setUint32(4, shape.length, true);
  view.setUint32(12, values.length, true);
  view.setBigUint64(16, BigInt(offset), true);
  view.setBigUint64(24, BigInt(total), true);
  shape.forEach((d, i) => view.setBigInt64(32 + i * 8, BigInt(d), true));
  values.forEach((v, i) => view.setFloat32(32 + shape.length * 8 + i * 4, v, true));
  return buffer;
}

function encodeIndices(total: number, sourceCloudId: bigint, values: number[]): ArrayBuffer {
  const buffer = new ArrayBuffer(24 + values.length * 4);
  const view = new DataView(buffer);
  view.setUint32(0, INDICES_MAGIC, true);
  view.setUint32(4, values.length, true);
  view.setUint32(8, total, true);
  view.setBigUint64(16, sourceCloudId, true);
  values.forEach((v, i) => view.setInt32(24 + i * 4, v, true));
  return buffer;
}

test("张量与下标的切片按契约解出来：形状是完整的、u64 的云 id 不丢精度", () => {
  assert.deepEqual(
    {
      tensor: decodeTensor(encodeTensor([2, 3, 4], 0, 24, [0.5, -1, 2])),
      indices: decodeIndices(encodeIndices(1000, 2n ** 63n + 5n, [7, 9, 42])),
      emptyTensor: decodeTensor(encodeTensor([8], 8, 8, [])),
    },
    {
      tensor: { shape: [2, 3, 4], total: 24, offset: 0, values: [0.5, -1, 2] },
      indices: { total: 1000, sourceCloudId: "9223372036854775813", values: [7, 9, 42] },
      // offset 越界是成功 + 空切片，不是错误
      emptyTensor: { shape: [8], total: 8, offset: 8, values: [] },
    },
  );
  const cut = encodeTensor([4], 0, 4, [1, 2, 3, 4]).slice(0, 40);
  assert.throws(() => decodeTensor(cut), /截断/);
});

test("图像载荷（LYIM）解出来；u16 拉伸到 8 位、超过边长按最近邻缩；PNG 头与像素经 inflate 读回一致", async () => {
  // 3x2 单通道 u16，第 1 行起的 2 行
  const buffer = new ArrayBuffer(48 + 12);
  const view = new DataView(buffer);
  [IMAGE_MAGIC, 3, 2, 1, 2, 0, 3, 2, 0, 2, 6, 0].forEach((v, i) => view.setUint32(i * 4, v, true));
  [0, 1000, 2000, 3000, 4000, 5000].forEach((v, i) => view.setUint16(48 + i * 2, v, true));
  const img = decodeImage(buffer);
  assert.deepEqual(
    { w: img.width, h: img.height, c: img.channels, depth: img.depth, rows: img.rowCount, values: [...img.values] },
    { w: 3, h: 2, c: 1, depth: 2, rows: 2, values: [0, 1000, 2000, 3000, 4000, 5000] },
  );
  assert.equal(levelToFit(3000, 1000, 768), 2);

  const pic = toPicture(3, 2, 1, 2, img.values, 1024);
  assert.deepEqual({ range: pic.range, bytes: [...pic.bytes] }, { range: [0, 5000], bytes: [0, 51, 102, 153, 204, 255] });
  assert.deepEqual([toPicture(3, 2, 1, 2, img.values, 2).width, toPicture(3, 2, 1, 2, img.values, 2).height], [2, 1]);

  const png = encodePng(pic);
  assert.deepEqual([...png.subarray(0, 8)], [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
  assert.equal(png.readUInt32BE(16), 3); // IHDR 宽
  assert.equal(png[25], 0); // 灰度
  const idatLen = png.readUInt32BE(33);
  assert.equal(png.toString("ascii", 37, 41), "IDAT");
  const { inflateSync } = await import("node:zlib");
  const raw = inflateSync(png.subarray(41, 41 + idatLen));
  assert.deepEqual([...raw], [0, 0, 51, 102, 0, 153, 204, 255]); // 每行前一个过滤器字节
  assert.throws(() => decodeImage(buffer.slice(0, 50)), /截断/);
});

test("view_output_image 按段取齐（fetchImage）：每段 4 行的 10 行图要三次，拼出来逐值相同；后续几段按帧头给的级别要", async () => {
  // 这条路以前没有测试（image-acceptance「留到以后」）：服务端按 16 MB 收行数，测试里的图都一段取完
  const width = 3;
  const height = 10;
  const asked: { level: number; row: number }[] = [];
  const get = async (level: number, row: number) => {
    asked.push({ level, row });
    const rows = Math.min(4, height - row);
    const buffer = new ArrayBuffer(48 + ((rows * width + 3) & ~3));
    const view = new DataView(buffer);
    // 帧头写实际给出的是第 0 级（HTTP 桩就是这样），后续几段要照它要
    [IMAGE_MAGIC, width, height, 1, 1, 0, width, height, row, rows, width, 0].forEach((v, i) =>
      view.setUint32(i * 4, v, true),
    );
    for (let i = 0; i < rows * width; i += 1) view.setUint8(48 + i, (row + Math.floor(i / width)) * 10 + (i % width));
    return buffer;
  };
  const { first, values } = await fetchImage(get, 2);
  assert.deepEqual(
    { asked, size: [first.width, first.height], values: [...values] },
    {
      asked: [{ level: 2, row: 0 }, { level: 0, row: 4 }, { level: 0, row: 8 }],
      size: [3, 10],
      values: Array.from({ length: width * height }, (_, i) => Math.floor(i / width) * 10 + (i % width)),
    },
  );
});
