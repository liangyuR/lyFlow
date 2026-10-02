// E8：文档缺 ui.position 时打开即布局（脚本生成的图必须能打开）。纯逻辑，不开浏览器 ——
// 原先在 scripts/e2e/m3.mjs 里动态 import 源文件验，打包模式下 import 不到只能放行，搬到这里。
// 画布上的 Ctrl+L 整理与撤销仍在 m3.mjs 的 suiteLayout 里走真实按键。
import assert from "node:assert/strict";
import { test } from "node:test";

import { layoutGraph, needsInitialLayout } from "../src/lib/layout.ts";
import { fitSidePanes } from "../src/lib/panes.ts";

const chain = (withPositions) => ({
  schemaVersion: 1,
  id: "x",
  nodes: [
    { id: "a", op: "gen.synthetic", ...(withPositions ? { ui: { position: { x: 0, y: 0 } } } : {}) },
    { id: "b", op: "filter.voxel_grid", ...(withPositions ? { ui: { position: { x: 300, y: 0 } } } : {}) },
  ],
  edges: [{ id: "e", from: { node: "a", port: "cloud" }, to: { node: "b", port: "cloud" } }],
});

test("缺坐标的文档会被判定为需要布局", () => {
  assert.equal(needsInitialLayout(chain(false)), true);
});

test("坐标齐全的文档不布局，空文档也不布局", () => {
  assert.equal(needsInitialLayout(chain(true)), false);
  assert.equal(needsInitialLayout({ schemaVersion: 1, id: "y", nodes: [], edges: [] }), false);
});

test("自动布局给每个节点一个落点，上游排在下游左边", () => {
  const moves = layoutGraph(chain(false));
  assert.equal(moves.length, 2);
  const at = Object.fromEntries(moves.map((m) => [m.id, m.position]));
  assert.ok(at.a.x < at.b.x, JSON.stringify(at));
});

// 三栏分宽度：窗口窄了两侧面板让位（右栏先缩），画布至少留 320。
// 修前两侧都不缩：1024 宽的窗口打开参数面板，画布挤成 0，面板右边一截跑到窗口外
test("窗口窄了两侧面板让位：右栏先缩到最窄，不够再缩左栏，画布留够最窄", () => {
  const palette = { width: 280, min: 180 };
  const panel = { width: 804, min: 360 };
  const cases = [
    ["放得下：原样", 1436, palette, { width: 380, min: 260 }, { left: 280, right: 380 }],
    ["只缩右栏就够", 1020, palette, panel, { left: 280, right: 420 }],
    ["右栏到最窄，再缩左栏", 896, palette, panel, { left: 216, right: 360 }],
    ["两侧都到最窄还放不下：停在最窄，画布让", 700, palette, panel, { left: 180, right: 360 }],
    ["还没量到宽度：原样", 0, palette, panel, { left: 280, right: 804 }],
  ];
  for (const [name, total, left, right, want] of cases) {
    assert.deepEqual(fitSidePanes(total, left, right, 320), want, name);
  }
});
