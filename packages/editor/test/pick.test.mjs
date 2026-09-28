// 预览里的选点与测距（lib/pick，docs/measure-plan.md）。纯逻辑；单击判定、标记点、readout 由 e2e m3.mjs 走真界面。
import assert from "node:assert/strict";
import { test } from "node:test";

import { addPick, measureLines, NO_MEASURE, pickCount, pickNearest } from "../src/lib/pick.ts";

// 单位阵（列主序）：x、y ∈ [-1, 1] 直接是 NDC，z 就是深度 —— 视口 100×100 时 (0, 0) 落在 (50, 50)
const I = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
// w = z：z ≤ 0 的点在「相机背后」
const W_IS_Z = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0];
const VP = { x: 0, y: 0, w: 100, h: 100 };

test("屏幕空间最近点：半径内取最近，同距取离相机近的，看不见的点跳过", () => {
  const cases = [
    // [说明, 点, 矩阵, 视口, 单击, 期望的下标]
    ["正中那个", [[0, 0, 0], [0.5, 0.5, 0]], I, VP, { x: 50, y: 50 }, 0],
    ["离光标 5 px 的那个（y 轴朝上：ny = 0.1 在屏幕上方）", [[0, 0.1, 0], [0.5, 0, 0]], I, VP, { x: 50, y: 46 }, 0],
    ["半径 8 px 外没有点", [[0.5, 0.5, 0]], I, VP, { x: 50, y: 50 }, null],
    ["前后叠着：取离相机近的", [[0, 0, 0.5], [0, 0, -0.5]], I, VP, { x: 50, y: 50 }, 1],
    ["远近平面之外的不算", [[0, 0, 2]], I, VP, { x: 50, y: 50 }, null],
    ["相机背后的不算", [[0, 0, -1], [0, 0, 1]], W_IS_Z, VP, { x: 50, y: 50 }, 1],
    ["NaN 槽跳过", [[Number.NaN, 0, 0], [0.02, 0, 0]], I, VP, { x: 50, y: 50 }, 1],
    ["对比的 B 栏：视口偏到右半边", [[0, 0, 0]], I, { x: 100, y: 0, w: 100, h: 100 }, { x: 150, y: 50 }, 0],
  ];
  const wrong = [];
  for (const [name, pts, m, vp, click, want] of cases) {
    const xyz = Float32Array.from(pts.flat());
    const got = pickNearest(xyz, pts.length, m, vp, click)?.index ?? null;
    if (got !== want) wrong.push([name, got, want]);
  }
  assert.deepEqual(wrong, [], "说明 / 实际 / 期望");
});

test("一组两点：第 3 次重新开始；readout 米 + 毫米并列，2D 多一行 XY 距离，对比标 A / B", () => {
  const p = (x, y, z, pane = 0) => ({ xyz: [x, y, z], pane });
  let m = addPick(NO_MEASURE, p(0, 0, 0));
  assert.equal(pickCount(m), 1);
  assert.deepEqual(measureLines(m, "3d").map((l) => [l.label, l.text]), [["P1", "(0, 0, 0) m"]]);

  m = addPick(m, p(0.003, 0.004, 0.012, 1));
  assert.equal(pickCount(m), 2);
  assert.deepEqual(measureLines(m, "3d", true).map((l) => [l.label, l.text]), [
    ["P1 · A", "(0, 0, 0) m"],
    ["P2 · B", "(0.003, 0.004, 0.012) m"],
    ["|d|", "0.013 m · 13 mm"],
    ["Δ", "(+0.003, +0.004, +0.012) m"],
  ]);
  assert.deepEqual(
    measureLines(m, "2d").find((l) => l.key === "distXY")?.text,
    "0.005 m · 5 mm",
    "XY 平面距离不管 z",
  );

  m = addPick(m, p(1, 1, 1));
  assert.deepEqual([pickCount(m), m.p1?.xyz], [1, [1, 1, 1]], "第 3 次点 = 新一组的 P1");
});
