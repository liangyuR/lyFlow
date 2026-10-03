// E8：文档缺 ui.position 时打开即布局（脚本生成的图必须能打开）。纯逻辑，不开浏览器 ——
// 原先在 scripts/e2e/m3.mjs 里动态 import 源文件验，打包模式下 import 不到只能放行，搬到这里。
// 画布上的 Ctrl+L 整理与撤销仍在 m3.mjs 的 suiteLayout 里走真实按键。
import assert from "node:assert/strict";
import { test } from "node:test";

import { layoutGraph, needsInitialLayout } from "../src/lib/layout.ts";
import { fitSidePanes } from "../src/lib/panes.ts";
import { branchSlot, estimateNodeHeight, placeMenu, revealShift } from "../src/lib/placement.ts";

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

// 右键菜单摆进窗口（1440 × 900 的窗口、离边 4 px）：放不下就翻到鼠标另一边，比窗口还高就贴顶限高。
// 修前照鼠标位置往右下摆，画布下半部分右键节点时菜单大半截在窗口外
test("右键菜单摆进窗口：放不下翻到鼠标另一边，比窗口还高就贴顶、限高", () => {
  const vp = { width: 1440, height: 900 };
  const menu = { width: 150, height: 300 };
  const cases = [
    ["放得下：照鼠标位置", { x: 100, y: 100 }, menu, { left: 100, top: 100 }],
    ["下面放不下：翻到鼠标上面", { x: 100, y: 800 }, menu, { left: 100, top: 500 }],
    ["右边放不下：翻到鼠标左边", { x: 1400, y: 100 }, menu, { left: 1250, top: 100 }],
    ["翻上去还放不下：贴着窗口顶", { x: 100, y: 500 }, { width: 150, height: 600 }, { left: 100, top: 4 }],
    ["比窗口还高：贴顶、限高（里面滚）", { x: 100, y: 400 }, { width: 150, height: 1000 }, { left: 100, top: 4, maxHeight: 892 }],
  ];
  for (const [name, at, size, want] of cases) {
    assert.deepEqual(placeMenu(at, size, vp), want, name);
  }
});

// 记在 localStorage 里的开关（检查器端口小节的开合）：只认 "1" / "0"，存储读不到、写不进都不报错
test("readStoredBool / writeStoredBool：坏值当没有，存储抛异常时照样往下走", async () => {
  const { readStoredBool, writeStoredBool } = await import("../src/lib/prefs.ts");
  const mem = new Map();
  globalThis.localStorage = { getItem: (k) => mem.get(k) ?? null, setItem: (k, v) => mem.set(k, String(v)) };
  try {
    assert.equal(readStoredBool("k"), null, "没存过");
    writeStoredBool("k", false);
    assert.deepEqual([mem.get("k"), readStoredBool("k")], ["0", false]);
    writeStoredBool("k", true);
    assert.equal(readStoredBool("k"), true);
    for (const bad of ["true", "", "2"]) {
      mem.set("k", bad);
      assert.equal(readStoredBool("k"), null, `存的是 ${JSON.stringify(bad)}`);
    }
    globalThis.localStorage = { getItem: () => { throw new Error("denied"); }, setItem: () => { throw new Error("quota"); } };
    assert.equal(readStoredBool("k"), null, "读不到");
    writeStoredBool("k", true); // 写不进：不抛
  } finally {
    delete globalThis.localStorage;
  }
});

// 键盘沿连线走时画布挪多少（lib/placement 的 revealShift）：在视野里不挪；不在就只挪最少的一点；放不下就居中
test("revealShift：节点移进视野只挪最少的一点，缩放不变；branchSlot：Tab 接出的新节点放在右边，压着谁就往下让", () => {
  const pane = { width: 800, height: 600 };
  const inset = { top: 40, right: 40, bottom: 40, left: 40 };
  const at1 = { x: 0, y: 0, zoom: 1 };
  const cases = [
    ["已经在视野里", at1, pane, { x: 100, y: 100, w: 200, h: 90 }, inset, null],
    ["出了右边：只横着挪", at1, pane, { x: 700, y: 100, w: 200, h: 90 }, inset, { x: 540, y: 300 }],
    ["压在顶上的面包屑下面：只竖着挪", at1, pane, { x: 100, y: 20, w: 200, h: 90 }, { ...inset, top: 72 }, { x: 400, y: 248 }],
    ["缩放 0.5：换算成 flow 坐标", { x: 0, y: 0, zoom: 0.5 }, pane, { x: 1600, y: 100, w: 200, h: 90 }, inset, { x: 1080, y: 600 }],
    ["画布比节点还窄：居中", at1, { width: 200, height: 600 }, { x: 300, y: 100, w: 200, h: 90 }, inset, { x: 400, y: 300 }],
  ];
  for (const [name, view, size, rect, pad, want] of cases) assert.deepEqual(revealShift(view, size, rect, pad), want, name);

  const anchor = { x: 0, y: 0, w: 200, h: 90 };
  const slots = [
    // [说明, 别的节点, 期望]
    ["右边空着：同一高度、隔 80", [], { x: 280, y: 0 }],
    ["右边是它接着的下游：往下让到它下面", [{ x: 300, y: 10, w: 220, h: 100 }], { x: 280, y: 150 }],
    ["下面一层又压着：接着往下", [{ x: 300, y: 10, w: 220, h: 100 }, { x: 260, y: 160, w: 200, h: 90 }], { x: 280, y: 290 }],
    ["只挨着、不压着：不让", [{ x: 480, y: 0, w: 200, h: 90 }, { x: 280, y: 90, w: 200, h: 90 }], { x: 280, y: 0 }],
    ["左边、上面的不管", [{ x: -300, y: 0, w: 200, h: 90 }, { x: 280, y: -200, w: 200, h: 90 }], { x: 280, y: 0 }],
  ];
  for (const [name, others, want] of slots) assert.deepEqual(branchSlot(anchor, others), want, `branchSlot：${name}`);
  // 新节点比选中的高（端口多）：按它自己的高度躲开下面的节点
  const below = [{ x: 300, y: 110, w: 220, h: 90 }];
  assert.deepEqual(branchSlot({ x: 0, y: 0, w: 200, h: 68 }, below), { x: 280, y: 0 }, "按选中的那个估：擦边过去");
  assert.deepEqual(branchSlot({ x: 0, y: 0, w: 200, h: 68 }, below, undefined, { h: estimateNodeHeight(4, 2) }), { x: 280, y: 240 },
    "四个输入的新节点 134 高：压着下面那个，往下让");
  assert.deepEqual([estimateNodeHeight(0, 1), estimateNodeHeight(1, 1), estimateNodeHeight(4, 2)], [68, 68, 134], "一行端口量出来是 68");
});
