import assert from "node:assert/strict";
import test from "node:test";

import { CLOUD_HAS_INTENSITY, CLOUD_MAGIC, decodeCloud, summarizeCloud } from "../src/cloud.js";

function encode(
  xyz: number[],
  options?: { intensity?: number[]; totalPoints?: number; bounds?: number[] },
): ArrayBuffer {
  const n = xyz.length / 3;
  const intensity = options?.intensity ?? null;
  const flags = intensity ? CLOUD_HAS_INTENSITY : 0;
  const bounds = options?.bounds ?? [
    Math.min(...xyz.filter((_, i) => i % 3 === 0)),
    Math.min(...xyz.filter((_, i) => i % 3 === 1)),
    Math.min(...xyz.filter((_, i) => i % 3 === 2)),
    Math.max(...xyz.filter((_, i) => i % 3 === 0)),
    Math.max(...xyz.filter((_, i) => i % 3 === 1)),
    Math.max(...xyz.filter((_, i) => i % 3 === 2)),
  ];
  const buffer = new ArrayBuffer(40 + n * 12 + (intensity ? n * 4 : 0));
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
  return buffer;
}

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
});
