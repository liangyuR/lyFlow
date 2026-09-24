// param-recipe P2.6：transform 与 curve 两种值的纯函数（lib/transform.ts、lib/curve.ts）。
// 判据与 core 的 checkCurveValue / evaluateCurve、transform.make 的旋转约定逐条对应。
import assert from "node:assert/strict";
import { test } from "node:test";

import {
  asCurve,
  curveProblem,
  evaluate,
  insertAt,
  insertInWidestGap,
  movePoint,
  normalized,
  removePoint,
  yRange,
} from "../src/lib/curve.ts";
import { valueEquals } from "../src/lib/params.ts";
import { asMatrix, compose, decompose, IDENTITY, isRigid, summarize } from "../src/lib/transform.ts";

const close = (a, b, eps = 1e-9) => Math.abs(a - b) <= eps;

test("transform：T·R 合成的是行主序刚体矩阵，平移在 m[3] m[7] m[11]", () => {
  const m = compose({ t: [0.1, -0.2, 0.3], r: [0, 0, 90] });
  assert.equal(m.length, 16);
  assert.deepEqual([m[3], m[7], m[11]], [0.1, -0.2, 0.3]);
  // 绕 Z 转 90°：x 轴 → y 轴，第一列是 (0, 1, 0)
  assert.deepEqual([m[0], m[4], m[8]], [0, 1, 0]);
  assert.deepEqual([m[12], m[13], m[14], m[15]], [0, 0, 0, 1]);
  assert.ok(isRigid(m));
});

test("transform：R = Rz·Ry·Rx（与 transform.make 同一约定），decompose 是 compose 的逆", () => {
  const cases = [
    [10, 20, 30],
    [-45, 5, 170],
    [0, -60, -120],
    [33.3, 12.5, -7.25],
  ];
  for (const r of cases) {
    const m = compose({ t: [1, 2, 3], r });
    const back = decompose(m);
    assert.deepEqual(back.t, [1, 2, 3]);
    for (let i = 0; i < 3; i += 1) assert.ok(close(back.r[i], r[i], 1e-7), `${r} → ${back.r}`);
    const again = compose(back);
    for (let i = 0; i < 16; i += 1) assert.ok(close(again[i], m[i], 1e-9), `m[${i}]`);
  }
  // 手算一个：只绕 X 转 90°，y 轴 → z 轴
  const rx = compose({ t: [0, 0, 0], r: [90, 0, 0] });
  assert.deepEqual([rx[1], rx[5], rx[9]], [0, 0, 1]);
});

test("transform：万向锁（ry = 90°）时 rx 取 0，矩阵照样还原", () => {
  const m = compose({ t: [0, 0, 0], r: [30, 90, 10] });
  const back = decompose(m);
  assert.equal(back.r[0], 0);
  assert.ok(close(back.r[1], 90, 1e-7));
  const again = compose(back);
  for (let i = 0; i < 16; i += 1) assert.ok(close(again[i], m[i], 1e-9));
});

test("transform：带缩放的不是刚体；坏值退回单位阵；摘要", () => {
  const scaled = [...IDENTITY];
  scaled[0] = 2;
  assert.equal(isRigid(scaled), false);
  assert.equal(summarize(scaled), "4×4 矩阵（含缩放或切变）");
  assert.deepEqual(asMatrix([1, 2, 3]), [...IDENTITY]);
  assert.deepEqual(asMatrix(null), [...IDENTITY]);
  assert.equal(summarize(compose({ t: [0.25, 0, -1], r: [0, 0, 45] })), "T[0.25, 0, -1] R[0, 0, 45]°");
});

test("curve：合法性与 core 同一套判据", () => {
  assert.equal(curveProblem({ points: [[0, 0], [1, 1]] }), null);
  assert.equal(curveProblem({ points: [[0, 0], [0.5, 0.2], [1, 1]], interp: "smooth" }), null);
  assert.match(curveProblem([0, 1]), /对象/);
  assert.match(curveProblem({ points: [[0, 0]] }), /两个控制点/);
  assert.match(curveProblem({ points: [[0, 0], [1.2, 1]] }), /\[0, 1\]/);
  assert.match(curveProblem({ points: [[0, 0], [0.5, 1], [0.5, 2]] }), /大于前一个点/);
  assert.match(curveProblem({ points: [[0, 0], [1, 1]], interp: "cubic" }), /interp/);
  assert.match(curveProblem({ points: [[0, 0], [1, 1]], tension: 1 }), /tension/);
  assert.match(curveProblem({ points: [[0, 0], [1, 1.5]] }, 0, 1), /不能大于 1/);
  assert.deepEqual(asCurve("坏的", { points: [[0, 1], [1, 0]] }).points, [[0, 1], [1, 0]]);
});

test("curve：线性插值、端点外取端点值；smooth 过控制点、不冲过", () => {
  const lin = { points: [[0.2, 1], [0.6, 3]] };
  assert.equal(evaluate(lin, 0.4), 2);
  assert.equal(evaluate(lin, 0), 1);
  assert.equal(evaluate(lin, 1), 3);
  const smooth = { interp: "smooth", points: [[0, 0], [0.5, 0.35], [0.6, 0.95], [1, 1]] };
  let prev = -1;
  for (let i = 0; i <= 100; i += 1) {
    const y = evaluate(smooth, i / 100);
    assert.ok(y >= prev - 1e-12 && y <= 1 + 1e-12);
    prev = y;
  }
  assert.ok(close(evaluate(smooth, 0.5), 0.35));
  assert.ok(close(evaluate(smooth, 0.6), 0.95));
});

test("curve：挪点夹在邻居与限位之间；插点线形不变；删点至少留两个", () => {
  const pts = [[0, 0], [0.5, 0.5], [1, 1]];
  assert.deepEqual(movePoint(pts, 1, 2, 3, 0, 1), [[0, 0], [0.999, 1], [1, 1]]);
  assert.deepEqual(movePoint(pts, 0, -1, -1, 0, 1), [[0, 0], [0.5, 0.5], [1, 1]]);
  const c = { points: pts, interp: "linear" };
  const hit = insertAt(c, 0.25);
  assert.equal(hit.index, 1);
  assert.deepEqual(hit.points[1], [0.25, 0.25]);
  assert.equal(insertAt(c, 0.5), null);
  assert.deepEqual(insertInWidestGap({ points: [[0, 0], [0.2, 0.2], [1, 1]] }).points[2], [0.6, 0.6]);
  assert.equal(removePoint([[0, 0], [1, 1]], 0), null);
  assert.deepEqual(removePoint(pts, 1), [[0, 0], [1, 1]]);
  assert.deepEqual(normalized([[0.1 + 0.2, 1]], "smooth"), { points: [[0.3, 1]], interp: "smooth" });
  assert.deepEqual(yRange({ points: [[0, -0.5], [1, 2]] }, {}), [-0.5, 2]);
  assert.deepEqual(yRange({ points: [[0, 0], [1, 1]] }, { softMin: 0, softMax: 10 }), [0, 10]);
});

test("valueEquals：curve 这样的对象按键比，与键的书写顺序无关", () => {
  const a = { points: [[0, 0], [1, 1]], interp: "linear" };
  const b = { interp: "linear", points: [[0, 0], [1, 1]] };
  assert.ok(valueEquals(a, b));
  assert.ok(!valueEquals(a, { points: [[0, 0], [1, 1]] }));
  assert.ok(!valueEquals(a, { ...a, points: [[0, 0], [1, 0.9]] }));
  assert.ok(!valueEquals(a, [a]));
});
