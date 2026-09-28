// 两节点输出对比的差异表（lib/compareDiff，docs/compare-plan.md §2）。纯逻辑；两栏场景、冻结、
// 表格的 DOM 标记由 e2e compare.mjs 走真界面。
import assert from "node:assert/strict";
import { test } from "node:test";

import { diffSides, withinTolerance } from "../src/lib/compareDiff.ts";

const side = (outputs, extra = {}) => ({ outputs, cloud: null, cloudPort: null, preview: false, ...extra });
const stat = (port, type, value, elementCount = 1) => ({ port, type, elementCount, ...(value ? { value } : {}) });
const cloud = (totalPoints, bounds, pointCount = totalPoints) => ({
  pointCount,
  totalPoints,
  bounds: Float32Array.from(bounds),
  xyz: new Float32Array(pointCount * 3),
  intensity: null,
  normals: null,
  rgb: null,
});
/** 只留测试关心的列：key → [a, b, delta, changed] */
const table = (result) => Object.fromEntries(result.rows.map((r) => [r.key, [r.a, r.b, r.delta, r.changed]]));

test("容差：|a − b| ≤ max(absTol, relTol · max(|a|, |b|))", () => {
  const cases = [
    // [说明, a, b, 期望相同]
    ["完全相等", 1, 1, true],
    ["零附近在绝对容差内", 0, 5e-10, true],
    ["零附近超出绝对容差", 0, 2e-9, false],
    ["大数在相对容差内", 1000, 1000.0009, true],
    ["大数超出相对容差", 1000, 1000.002, false],
    ["NaN 与数字", Number.NaN, 1, false],
  ];
  const wrong = cases
    .map(([name, a, b, want]) => [name, withinTolerance(a, b), want])
    .filter(([, got, want]) => got !== want);
  assert.deepEqual(wrong, [], "说明 / 实际 / 期望");
});

test("每种类型摊开成哪些行、Δ = A − B 怎么写", () => {
  const cases = [
    // [说明, A 的输出, B 的输出, 期望的 key → [a, b, delta, changed]（只列关心的行）]
    [
      "Box2D：两角 + 尺寸 + 中心",
      [stat("roi", "Box2D", { kind: "Box2D", min: [0, 0], max: [2, 4] })],
      [stat("roi", "Box2D", { kind: "Box2D", min: [0, 0], max: [2, 3] })],
      {
        "roi.min": ["(0, 0)", "(0, 0)", "(0, 0)", false],
        "roi.max": ["(2, 4)", "(2, 3)", "(0, +1)", true],
        "roi.size": ["(2, 4)", "(2, 3)", "(0, +1)", true],
        "roi.center": ["(1, 2)", "(1, 1.5)", "(0, +0.5)", true],
      },
    ],
    [
      "Line2D 无线段：point + dir",
      [stat("l", "Line2D", { kind: "Line2D", point: [0, 1], dir: [1, 0], hasSegment: false })],
      [stat("l", "Line2D", { kind: "Line2D", point: [0, 1], dir: [1, 0], hasSegment: false })],
      { "l.point": ["(0, 1)", "(0, 1)", "(0, 0)", false], "l.dir": ["(1, 0)", "(1, 0)", "(0, 0)", false] },
    ],
    [
      "Line2D 有线段：start + end + length",
      [stat("l", "Line2D", { kind: "Line2D", hasSegment: true, start: [0, 0], end: [3, 4] })],
      [stat("l", "Line2D", { kind: "Line2D", hasSegment: true, start: [0, 0], end: [0, 4] })],
      { "l.end": ["(3, 4)", "(0, 4)", "(+3, 0)", true], "l.length": ["5", "4", "+1", true] },
    ],
    [
      "Circle2D",
      [stat("c", "Circle2D", { kind: "Circle2D", center: [1, 1], radius: 2 })],
      [stat("c", "Circle2D", { kind: "Circle2D", center: [1, 1], radius: 2.5 })],
      { "c.center": ["(1, 1)", "(1, 1)", "(0, 0)", false], "c.radius": ["2", "2.5", "-0.5", true] },
    ],
    [
      "Point2D",
      [stat("pt", "Point2D", { kind: "Point2D", p: [1, 2] })],
      [stat("pt", "Point2D", { kind: "Point2D", p: [1, 2] })],
      { "pt.p": ["(1, 2)", "(1, 2)", "(0, 0)", false] },
    ],
    [
      "Measurement：带单位的数值 + 文本",
      [stat("gap", "Measurement", { kind: "Measurement", value: 4.014, unit: "mm", ok: true, verdict: "OK", nominal: 4 })],
      [stat("gap", "Measurement", { kind: "Measurement", value: 4, unit: "mm", ok: false, verdict: "NG", nominal: 4 })],
      {
        "gap.value": ["4.014 mm", "4 mm", "+0.014 mm", true],
        "gap.nominal": ["4 mm", "4 mm", "0", false],
        "gap.ok": ["true", "false", null, true],
        "gap.verdict": ["OK", "NG", null, true],
      },
    ],
    [
      "Measurement：没测出（null）与数字比算不同，Δ 为 —",
      [stat("gap", "Measurement", { kind: "Measurement", value: null, unit: "mm" })],
      [stat("gap", "Measurement", { kind: "Measurement", value: 4, unit: "mm" })],
      { "gap.value": ["—", "4 mm", "—", true] },
    ],
    [
      "Measurement：两侧都没测出算相同",
      [stat("gap", "Measurement", { kind: "Measurement", value: null, unit: "mm" })],
      [stat("gap", "Measurement", { kind: "Measurement", value: null, unit: "mm" })],
      { "gap.value": ["—", "—", null, false] },
    ],
    [
      "Measurement：单位不同算不同",
      [stat("gap", "Measurement", { kind: "Measurement", value: 4, unit: "mm" })],
      [stat("gap", "Measurement", { kind: "Measurement", value: 0.004, unit: "m" })],
      { "gap.value": ["4 mm", "0.004 m", "单位不同", true] },
    ],
    [
      "Record：逐键递归，数字 / 数字数组 / 其余文本；超过深度 2 整块按文本",
      [stat("info", "Record", { kind: "Record", data: { score: 0.9, xy: [1, 2], name: "a", deep: { k: 1 }, far: { x: { y: 1 } } } })],
      [stat("info", "Record", { kind: "Record", data: { score: 0.8, xy: [1, 2], name: "a", deep: { k: 1 }, far: { x: { y: 2 } }, extra: 3 } })],
      {
        "info.data.score": ["0.9", "0.8", "+0.1", true],
        "info.data.xy": ["(1, 2)", "(1, 2)", "(0, 0)", false],
        "info.data.name": ["a", "a", null, false],
        "info.data.deep.k": ["1", "1", "0", false],
        "info.data.far.x": ['{"y":1}', '{"y":2}', null, true],
        "info.data.extra": ["—", "3", "—", true],
      },
    ],
    [
      "Plane",
      [stat("pl", "Plane", { kind: "Plane", normal: [0, 0, 1], d: 0.5 })],
      [stat("pl", "Plane", { kind: "Plane", normal: [0, 0, 1], d: 0.5 })],
      { "pl.normal": ["(0, 0, 1)", "(0, 0, 1)", "(0, 0, 0)", false], "pl.d": ["0.5", "0.5", "0", false] },
    ],
    [
      "Transform：Δ 只写最大绝对差",
      [stat("t", "Transform", { kind: "Transform", m: [1, 0, 0, 0.3, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1] })],
      [stat("t", "Transform", { kind: "Transform", m: [1, 0, 0, 0.1, 0, 1, 0, -0.1, 0, 0, 1, 0, 0, 0, 0, 1] })],
      { "t.m": [undefined, undefined, "max|Δ| 0.2", true] },
    ],
    [
      "Tensor：形状按文本，统计量按数字",
      [stat("x", "Tensor", { kind: "Tensor", shape: [1, 3], count: 3, min: 0, max: 1, mean: 0.5 })],
      [stat("x", "Tensor", { kind: "Tensor", shape: [1, 4], count: 4, min: 0, max: 1, mean: 0.5 })],
      {
        "x.shape": ["[1, 3]", "[1, 4]", null, true],
        "x.count": ["3", "4", "-1 (-25%)", true],
        "x.mean": ["0.5", "0.5", "0", false],
      },
    ],
    [
      "Indices：只比个数，带百分比",
      [stat("idx", "Indices", undefined, 2_001_880)],
      [stat("idx", "Indices", undefined, 2_000_000)],
      { "idx.count": ["2 001 880", "2 000 000", "+1 880 (+0.094%)", true] },
    ],
    [
      "Error",
      [stat("err", "Error", { kind: "Error", message: "boom" })],
      [stat("err", "Error", { kind: "Error", message: "boom" })],
      { "err.message": ["boom", "boom", null, false] },
    ],
    [
      "不认识的类型只比 elementCount",
      [stat("z", "Whatever", undefined, 3)],
      [stat("z", "Whatever", undefined, 3)],
      { "z.elementCount": ["3", "3", "0", false] },
    ],
  ];
  const wrong = [];
  for (const [name, outA, outB, want] of cases) {
    const got = table(diffSides(side(outA), side(outB)));
    for (const [key, [a, b, delta, changed]] of Object.entries(want)) {
      const row = got[key];
      const actual = row && [a === undefined ? a : row[0], b === undefined ? b : row[1], row[2], row[3]];
      if (JSON.stringify(actual) !== JSON.stringify([a, b, delta, changed])) wrong.push([name, key, actual, [a, b, delta, changed]]);
    }
  }
  assert.deepEqual(wrong, [], "说明 / 行 / 实际 / 期望");
});

test("点云：点数取载荷头的全量 totalPoints，包围盒取全量 bounds；没取到云退回 elementCount", () => {
  const out = [stat("cloud", "PointCloud", undefined, 100)];
  // 显示抽稀到 50 点，但全量是 1000 / 800
  const a = side(out, { cloud: cloud(1000, [0, 0, 0, 1, 1, 1], 50), cloudPort: "cloud" });
  const b = side(out, { cloud: cloud(800, [0, 0, 0, 1, 1, 2], 50), cloudPort: "cloud" });
  const t = table(diffSides(a, b));
  assert.deepEqual(t["cloud.points"], ["1 000", "800", "+200 (+25%)", true]);
  assert.deepEqual(t["cloud.bounds.max"], ["(1, 1, 1)", "(1, 1, 2)", "(0, 0, -1)", true]);
  assert.deepEqual(t["cloud.size"], ["(1, 1, 1)", "(1, 1, 2)", "(0, 0, -1)", true]);

  // B 侧没取到云（还在取、或那片云是别的端口的）：点数用 elementCount，包围盒「—」且不算不同
  const noCloud = table(diffSides(a, side(out, { cloud: cloud(5, [0, 0, 0, 9, 9, 9]), cloudPort: "other" })));
  assert.deepEqual(noCloud["cloud.points"], ["1 000", "100", "+900 (+900%)", true]);
  assert.deepEqual(noCloud["cloud.bounds.min"], ["(0, 0, 0)", "—", "—", true]);
  const neither = table(diffSides(side(out), side(out)));
  assert.deepEqual(neither["cloud.points"], ["100", "100", "0", false]);
  assert.equal(neither["cloud.bounds.min"], undefined, "两侧都没云：包围盒不出行");
});

test("端口配对：同名优先，其次（类型，出现顺序），剩下的只计数；Bundle 只比字段项", () => {
  const m = (value) => ({ kind: "Measurement", value, unit: "mm" });
  const a = side([
    stat("gap", "Measurement", m(4)),
    stat("left", "Box2D", { kind: "Box2D", min: [0, 0], max: [1, 1] }),
    stat("note", "Record", { kind: "Record", data: {} }),
    stat("scan", "Bundle<gap.ScanPair>", undefined, 3),
    stat("scan.merged", "PointCloud", undefined, 10),
  ]);
  const b = side([
    stat("roi", "Box2D", { kind: "Box2D", min: [0, 0], max: [1, 1] }),
    stat("gap", "Measurement", m(4)),
    stat("err", "Error", { kind: "Error", message: "x" }),
    stat("scan", "Bundle<gap.ScanPair>", undefined, 3),
    stat("scan.merged", "PointCloud", undefined, 12),
  ]);
  const r = diffSides(a, b);
  const keys = r.rows.map((row) => row.key);
  assert.ok(keys.includes("gap.value"), "同名配对");
  assert.ok(keys.includes("left/roi.max"), "按类型配上，键写两边的端口名");
  assert.deepEqual(
    r.rows.filter((row) => row.kind === "only").map((row) => [row.key, row.side]),
    [["note", "A"], ["err", "B"]],
  );
  assert.ok(keys.includes("scan.merged.points"), "Bundle 的点云字段");
  assert.ok(!keys.some((k) => k.startsWith("scan.elementCount")), "整端口那条跳过");
  assert.equal(r.only, 2);
  assert.equal(r.changed, 1, "只有 scan.merged 的点数不同；only 行不算不同");
  assert.equal(r.changed + r.same + r.only, r.rows.length);
});

test("模式不同（一侧预览、一侧正式）打标记", () => {
  assert.equal(diffSides(side([]), side([])).modeMismatch, false);
  assert.equal(diffSides(side([], { preview: true }), side([])).modeMismatch, true);
  assert.deepEqual(diffSides(side(undefined), side(undefined)).rows, [], "还没有输出：空表");
});
