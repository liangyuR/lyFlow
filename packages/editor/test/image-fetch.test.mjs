// 一级图像按段取齐（lib/imageFetch）：桥接层按 16 MB 收行数，大图要分几段取。
// 这条路以前没有测试覆盖（image-acceptance「留到以后」）—— 真 app 里的图都一段取完。这里用假的取数函数，
// 每段只给 4 行，验拼接位置、类型化数组的位深与「照帧头的 rowCount 接着要」。
import assert from "node:assert/strict";
import { test } from "node:test";

import { fetchImageLevel } from "../src/lib/imageFetch.ts";
import { IMAGE_HEADER_BYTES, IMAGE_MAGIC } from "../src/types/execution.ts";

/** 一段 LYIM 载荷：值 = 行号 × 100 + 列号 × channels + 通道（与像素位置一一对应，拼错一行就对不上）。 */
function segment({ width, height, channels, depth, level = 0 }, rowOffset, rowCount) {
  const rows = Math.max(0, Math.min(rowCount, height - rowOffset));
  const rowBytes = width * channels * depth;
  const padded = (rows * rowBytes + 3) & ~3;
  const buffer = new ArrayBuffer(IMAGE_HEADER_BYTES + padded);
  const head = new DataView(buffer);
  [IMAGE_MAGIC, width, height, channels, depth, level, width, height, rowOffset, rows, rowBytes, 0].forEach((v, i) =>
    head.setUint32(i * 4, v, true),
  );
  const n = rows * width * channels;
  const Typed = depth === 1 ? Uint8Array : depth === 2 ? Uint16Array : Float32Array;
  const px = new Typed(buffer, IMAGE_HEADER_BYTES, n);
  for (let i = 0; i < n; i += 1) {
    const y = rowOffset + Math.floor(i / (width * channels));
    px[i] = (y * 100 + (i % (width * channels))) % (depth === 1 ? 256 : 65536);
  }
  return buffer;
}

function expected({ width, height, channels, depth }) {
  return Array.from({ length: width * height * channels }, (_, i) => {
    const y = Math.floor(i / (width * channels));
    return (y * 100 + (i % (width * channels))) % (depth === 1 ? 256 : 65536);
  });
}

/** 假的 getOutputImage：每次最多给 maxRows 行，记下被要了哪几段。 */
function fakeGet(spec, maxRows) {
  const asked = [];
  const get = async (runId, nodeId, port, level, row, rows) => {
    asked.push({ row, rows, level });
    return segment(spec, row, maxRows);
  };
  return { get, asked };
}

test("分段取齐：每段 4 行的 10 行图要三次（0、4、8 起），拼出来逐个通道值与原图相同", async () => {
  for (const spec of [
    { width: 5, height: 10, channels: 1, depth: 1 },
    { width: 3, height: 10, channels: 3, depth: 2 },
    { width: 4, height: 10, channels: 1, depth: 4 },
  ]) {
    const { get, asked } = fakeGet(spec, 4);
    const img = await fetchImageLevel(get, "r", "n", "image", 0);
    assert.deepEqual(
      { size: [img.width, img.height, img.channels], rows: asked.map((a) => a.row), type: img.pixels.constructor.name },
      {
        size: [spec.width, spec.height, spec.channels],
        rows: [0, 4, 8],
        type: spec.depth === 1 ? "Uint8Array" : spec.depth === 2 ? "Uint16Array" : "Float32Array",
      },
      JSON.stringify(spec),
    );
    assert.deepEqual(Array.from(img.pixels), expected(spec), JSON.stringify(spec));
  }
});

test("一段就取完的图只要一次；给出的级别照帧头（要得太大时 core 收到 1 像素那级）", async () => {
  const spec = { width: 2, height: 3, channels: 1, depth: 1, level: 7 };
  const { get, asked } = fakeGet(spec, 100);
  const img = await fetchImageLevel(get, "r", "n", "image", 9);
  assert.deepEqual({ level: img.level, asked: asked.length, firstLevel: asked[0].level }, { level: 7, asked: 1, firstLevel: 9 });
});
