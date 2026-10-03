// 主预览按节点的输出类型选内容（lib/viewRule）：点云场景还是值的表格。
// 纯逻辑；连线查看器按端口选视图的那半（defaultViewFor）由 e2e peek.mjs / m8b.mjs 走真界面。
import assert from "node:assert/strict";
import { test } from "node:test";

import { compareContentFor, viewerContentFor } from "../src/lib/viewRule.ts";

const bundles = [
  { kind: "gap.ScanPair", fields: [{ name: "primary", type: "PointCloud" }] },
  { kind: "t.Info", fields: [{ name: "info", type: "Record" }, { name: "m", type: "Measurement" }] },
  { kind: "t.Rois", fields: [{ name: "datum", type: "Box2D" }, { name: "info", type: "Record" }] },
];

test("主预览：有一个可画的端口就显示点云场景，全是值才换成表格", () => {
  const cases = [
    // [说明, 输出类型, 期望, 输入类型（可选）]
    ["点云", ["PointCloud"], "cloud"],
    ["只有 2D 几何：叠在上游借来的底图上", ["Box2D", "Line2D"], "cloud"],
    ["Indices 指向一片云，底图有意义", ["Indices", "Plane"], "cloud"],
    ["点云 + 量测：点云优先", ["PointCloud", "Measurement"], "cloud"],
    ["只有 Record（字符串 / JSON）", ["Record"], "value"],
    ["量测与平面", ["Measurement", "Plane"], "value"],
    ["张量在主预览里只看得到值", ["Tensor"], "value"],
    ["输出图像：图像模式", ["Image"], "image"],
    ["图像域的像素几何（找圆 / 区域统计）：画在输入那张图上", ["Measurement", "Box2D"], "image", ["Image", "Image"]],
    ["输入是图像、输出是点云（深度图转点云）：点云优先", ["PointCloud"], "cloud", ["Image"]],
    ["没有图像的 2D 几何照旧叠在点云底图上", ["Box2D"], "cloud", ["PointCloud"]],
    ["Bundle 里有点云字段", ["Bundle<gap.ScanPair>"], "cloud"],
    ["Bundle 里有 2D 几何字段", ["Bundle<t.Rois>"], "cloud"],
    ["Bundle 里全是值", ["Bundle<t.Info>"], "value"],
    ["不认识的 Bundle 按老行为走点云场景", ["Bundle<t.Unknown>"], "cloud"],
    ["Any 还没跑过，类型未知：按老行为", ["Any", "Record"], "cloud"],
    ["没有输出端口", [], "cloud"],
  ];
  const wrong = cases
    .map(([name, types, want, inputs]) => [name, viewerContentFor(types, bundles, inputs), want])
    .filter(([, got, want]) => got !== want);
  assert.deepEqual(wrong, [], "说明 / 实际 / 期望");
});

test("对比的两栏：一侧可画就是点云场景，两侧都只有值才是值表格", () => {
  const got = [["cloud", "cloud"], ["cloud", "value"], ["value", "cloud"], ["value", "value"]].map(
    ([a, b]) => compareContentFor(a, b),
  );
  assert.deepEqual(got, ["cloud", "cloud", "cloud", "value"]);
});

// 换了一片云要不要重新取景（lib/viewFit）。修前每片新云都取景：重跑、在链上逐个点节点都把视角拉回斜 45°
test("sameFrame：包围盒相交、对角线差不到 4 倍才算同一个坐标系（不重新取景）", async () => {
  const { sameFrame } = await import("../src/lib/viewFit.ts");
  const box = (x0, y0, z0, x1, y1, z1) => [x0, y0, z0, x1, y1, z1];
  const base = box(0, 0, 0, 10, 10, 2);
  const cases = [
    ["同一片", base, true],
    ["抽稀之后小一圈", box(0.2, 0.1, 0, 9.8, 9.9, 1.9), true],
    ["只截了一部分（大小差 3 倍）", box(0, 0, 0, 3.5, 3.5, 1), true],
    ["平移 100 m", box(100, 0, 0, 110, 10, 2), false],
    ["放大 1000 倍", box(0, 0, 0, 10000, 10000, 2000), false],
    ["小到 1/10（差 10 倍）", box(0, 0, 0, 1, 1, 0.2), false],
    ["退化成一个点、与一片云", box(5, 5, 1, 5, 5, 1), false],
  ];
  for (const [name, other, want] of cases) assert.equal(sameFrame(base, other), want, name);
});

// 标准视角（lib/viewFit 的 presetPosition）：绕着现在的转心转过去、距离不变
test("presetPosition：绕同一个转心、距离不变；俯视屏幕上方是 +Y；轴测与 ⤢ 同一个方向", async () => {
  const { ISO_DIR, presetPosition } = await import("../src/lib/viewFit.ts");
  const target = [1, 2, 3];
  const from = [4, -2, 3]; // 离转心 5
  const unit = (v) => {
    const n = Math.hypot(...v);
    return v.map((x) => x / n);
  };
  const iso = unit(ISO_DIR);
  const cases = [
    // [视角, 期望的方向（从转心指向相机）]
    ["front", [0, -1, 0]],
    ["side", [1, 0, 0]],
    ["iso", iso],
  ];
  for (const [view, dir] of cases) {
    const p = presetPosition(view, target, from);
    const got = unit([p[0] - target[0], p[1] - target[1], p[2] - target[2]]);
    assert.ok(got.every((v, i) => Math.abs(v - dir[i]) < 1e-9), `${view}：${got}`);
    assert.ok(Math.abs(Math.hypot(p[0] - 1, p[1] - 2, p[2] - 3) - 5) < 1e-9, `${view}：距离不变`);
  }
  const top = presetPosition("top", target, from);
  assert.ok(top[0] === 1 && top[1] < 2 && top[1] > 1.99 && top[2] > 7.99, `俯视：正上方、往 −Y 偏一丝（屏幕上方 = +Y）：${top}`);
});

// 预览的显示设置（lib/viewPrefs）：读回存着的那一份时逐项校验；手动着色范围按着色模式分开记
test("显示设置读回：坏的那一项回到默认，别的照用", async () => {
  const { DEFAULT_VIEWER_PREFS: d, parseViewerPrefs } = await import("../src/lib/viewPrefs.ts");
  const good = { shading: "height", ramp: "jet", pointSize: 2.4, maxPoints: 500_000 };
  const cases = [
    ["没存过", null, d],
    ["不是 JSON", "{oops", d],
    ["是数组", "[1,2]", d],
    ["全对", JSON.stringify(good), good],
    ["未知的着色名", JSON.stringify({ ...good, shading: "rainbow" }), { ...good, shading: d.shading }],
    ["原型链上的名字不算", JSON.stringify({ ...good, ramp: "toString" }), { ...good, ramp: d.ramp }],
    ["点大小越界：夹到 0.5–6", JSON.stringify({ ...good, pointSize: 40 }), { ...good, pointSize: 6 }],
    ["点大小不是数", JSON.stringify({ ...good, pointSize: "big" }), { ...good, pointSize: d.pointSize }],
    ["显示点数不在可选项里", JSON.stringify({ ...good, maxPoints: 123 }), { ...good, maxPoints: d.maxPoints }],
  ];
  for (const [name, raw, want] of cases) assert.deepEqual(parseViewerPrefs(raw), want, name);
});

test("手动着色范围：只改当前着色那一份，另一个界取当前值；「自动」只清当前那一份", async () => {
  const { rangeFor, withRangeAuto, withRangeEnd } = await import("../src/lib/viewPrefs.ts");
  const autoI = [0, 255];
  const autoH = [-1, 3];
  let m = {};
  assert.deepEqual(rangeFor(m, "intensity", autoI), { range: autoI, auto: true });
  m = withRangeEnd(m, "intensity", 1, 100, autoI);
  assert.deepEqual(rangeFor(m, "intensity", autoI), { range: [0, 100], auto: false }, "填上界：下界取当时的自动值");
  assert.deepEqual(rangeFor(m, "height", autoH), { range: autoH, auto: true }, "切到高度：还是高度自己的自动范围");
  m = withRangeEnd(m, "height", 0, 0.5, autoH);
  m = withRangeAuto(m, "height");
  assert.deepEqual([rangeFor(m, "height", autoH).auto, rangeFor(m, "intensity", autoI).range], [true, [0, 100]],
    "高度回到自动，强度那一份还在");
  assert.equal(withRangeAuto(m, "normal"), m, "本来就是自动：原样返回");

  // 范围框的小数位随跨度走（米为单位）：亚毫米的高度范围两端不能显示成同一个数，取整差不过跨度的千分之一
  const { rangeDigits, roundTo } = await import("../src/lib/viewPrefs.ts");
  for (const [lo, hi, want] of [[0, 255, 3], [0, 1, 3], [0.0101, 0.0104, 7], [-0.0003, 0.0004, 7], [5, 5, 3], [0, Infinity, 3]]) {
    assert.equal(rangeDigits(lo, hi), want, `${lo}–${hi}`);
  }
  const d = rangeDigits(0.0101, 0.0104);
  assert.ok(roundTo(0.0101, d) < roundTo(0.0104, d), "两端显示得出差别");
});

test("启动时从 localStorage 读回显示设置，改了就写回去", async () => {
  const { VIEWER_PREFS_KEY } = await import("../src/lib/viewPrefs.ts");
  const stored = { shading: "height", ramp: "gray", pointSize: 3, maxPoints: 500_000 };
  const mem = new Map([[VIEWER_PREFS_KEY, JSON.stringify(stored)]]);
  globalThis.localStorage = { getItem: (k) => mem.get(k) ?? null, setItem: (k, v) => mem.set(k, String(v)), removeItem: (k) => mem.delete(k) };
  try {
    // 这个文件里别处没引过 ui store：这里才第一次加载，初值就从上面这份存储里读
    const { useUiStore } = await import("../src/store/ui.ts");
    assert.deepEqual(useUiStore.getState().viewerPrefs, stored);
    useUiStore.getState().setViewerPrefs({ pointSize: 2 });
    assert.deepEqual(JSON.parse(mem.get(VIEWER_PREFS_KEY)), { ...stored, pointSize: 2 });
  } finally {
    delete globalThis.localStorage;
  }
});

// 预览选看哪个点云输出（lib/basecloud）：列出全部点云口；几何节点的底图跟着它接的那个口走
test("cloudPortsOf：点云口按声明顺序，再是 Bundle 里的点云字段；底图沿边取它接的那个口", async () => {
  const { cloudPortsOf, findBaseCloud } = await import("../src/lib/basecloud.ts");
  const port = (name, type) => ({ name, type });
  const ops = new Map([
    ["seg.extract", { id: "seg.extract", inputs: [port("cloud", "PointCloud")], outputs: [port("selected", "PointCloud"), port("rest", "PointCloud")] }],
    ["io.pair", { id: "io.pair", inputs: [], outputs: [port("info", "Record"), port("pair", "Bundle<t.ScanPair>")] }],
    ["fit.line", { id: "fit.line", inputs: [port("cloud", "PointCloud")], outputs: [port("line", "Line2D")] }],
  ]);
  const bundles = [{ kind: "t.ScanPair", fields: [{ name: "primary", type: "PointCloud" }, { name: "meta", type: "Record" }, { name: "merged", type: "PointCloud" }] }];
  for (const [opId, want] of [
    ["seg.extract", ["selected", "rest"]],
    ["io.pair", ["pair.primary", "pair.merged"]],
    ["fit.line", []],
    ["no.such", []],
  ]) {
    assert.deepEqual(cloudPortsOf(ops, opId, bundles), want, opId);
  }

  const doc = (fromPort) => ({
    schemaVersion: 1,
    nodes: [{ id: "x", op: "seg.extract", params: {} }, { id: "f", op: "fit.line", params: {} }],
    edges: [{ id: "e", from: { node: "x", port: fromPort }, to: { node: "f", port: "cloud" } }],
  });
  assert.equal(findBaseCloud(doc("rest"), [], "f", ops, bundles)?.resolved.port, "rest", "接在 rest 上：底图是 rest（以前是 selected）");
  assert.equal(findBaseCloud(doc("selected"), [], "f", ops, bundles)?.resolved.port, "selected");
});
