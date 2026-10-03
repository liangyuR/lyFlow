// 主预览按节点的输出类型选内容（lib/viewRule）：点云场景还是值的表格。
// 纯逻辑；连线查看器按端口选视图的那半（defaultViewFor）由 e2e peek.mjs / m8b.mjs 走真界面。
import assert from "node:assert/strict";
import { test } from "node:test";

import { formatOutputValue, readingsOf, sortSummaryOutputs } from "../src/lib/outputs.ts";
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

// 预览的网格跟着云走（lib/viewFit 的 gridSpec）
test("gridSpec：格子 1/2/5、云上 10–25 格、居中在整格上、铺在最低处、没重新取景时不换档", async () => {
  const { gridSpec } = await import("../src/lib/viewFit.ts");
  const cases = [
    // [说明, 包围盒, 上一次的格子, 期望的格子, 期望的文字]
    ["1.2 m 的云", [0, 0, 0, 1.2, 0.5, 0.25], null, 0.1, "格 100 mm"],
    ["±5 cm（Float32 的尾巴不露出来）", Float32Array.from([-0.05, -0.05, 0, 0.05, 0.05, 0.02]), null, 0.01, "格 10 mm"],
    ["x≈1000 处的 12 mm 小零件", Float32Array.from([1000, 0, 0, 1000.012, 0.008, 0.003]), null, 0.001, "格 1 mm"],
    ["12 km", [0, 0, 0, 12000, 500, 30], null, 1000, "格 1000 m"],
    ["一个点", [1, 2, 3, 1, 2, 3], null, 0.001, "格 1 mm"],
    ["竖着的一条线（XY 没有跨度）", [0, 0, 0, 0, 0, 2], null, 0.2, "格 200 mm"],
    ["没重新取景：云上还有 6–40 格就不换档", [0, 0, 0, 0.0999, 0.05, 0], 0.01, 0.01, "格 10 mm"],
    ["没重新取景：差太远了照样换档", [0, 0, 0, 0.8, 0.5, 0], 0.01, 0.05, "格 50 mm"],
  ];
  const wrong = [];
  for (const [name, b, keep, cell, text] of cases) {
    const g = gridSpec(b, keep);
    if (!g || Math.abs(g.cell - cell) > cell * 1e-9 || g.text !== text) {
      wrong.push([name, g && { cell: g.cell, text: g.text }, { cell, text }]);
      continue;
    }
    // 中心落在整格上、格数是偶数（每条线都在整格坐标上）、四周至少多出一格、铺在最低点下面一丝
    const onCell = g.center.every((c) => Math.abs(c / g.cell - Math.round(c / g.cell)) < 1e-6);
    const half = (g.cell * g.divisions) / 2;
    const covers = g.center[0] - half <= b[0] - g.cell + 1e-9 && g.center[0] + half >= b[3] + g.cell - 1e-9 &&
      g.center[1] - half <= b[1] - g.cell + 1e-9 && g.center[1] + half >= b[4] + g.cell - 1e-9;
    const under = g.z <= b[2] && g.z >= b[2] - g.cell / 100;
    if (!onCell || g.divisions % 2 !== 0 || !covers || !under) wrong.push([name, g, "不变式"]);
  }
  assert.deepEqual(wrong, [], "说明 / 实际 / 期望");
  assert.equal(gridSpec([NaN, 0, 0, 1, 1, 1]), null, "NaN：不动");
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
test("cloudPortsOf：点云口按声明顺序，再是 Bundle 里的点云字段；底图沿边取它接的那个口；出错节点的输入先取报错指着的那个口", async () => {
  const { cloudPortsOf, findBaseCloud } = await import("../src/lib/basecloud.ts");
  const port = (name, type) => ({ name, type });
  const ops = new Map([
    ["seg.extract", { id: "seg.extract", inputs: [port("cloud", "PointCloud")], outputs: [port("selected", "PointCloud"), port("rest", "PointCloud")] }],
    ["io.pair", { id: "io.pair", inputs: [], outputs: [port("info", "Record"), port("pair", "Bundle<t.ScanPair>")] }],
    ["fit.line", { id: "fit.line", inputs: [port("cloud", "PointCloud")], outputs: [port("line", "Line2D")] }],
    ["m.flush", { id: "m.flush", inputs: [port("base", "PointCloud"), port("ref", "PointCloud"), port("hint", "Line2D")], outputs: [port("value", "Measurement")] }],
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

  // 两个点云输入的量测节点出错：报错指着哪个口就画那个口接的云；没指、指的口没接东西都照旧按声明顺序取第一个
  const two = {
    schemaVersion: 1,
    nodes: [{ id: "a", op: "seg.extract", params: {} }, { id: "b", op: "seg.extract", params: {} }, { id: "m", op: "m.flush", params: {} }],
    edges: [
      { id: "e1", from: { node: "a", port: "selected" }, to: { node: "m", port: "base" } },
      { id: "e2", from: { node: "b", port: "rest" }, to: { node: "m", port: "ref" } },
    ],
  };
  for (const [via, want] of [[undefined, "a.selected"], ["ref", "b.rest"], ["base", "a.selected"], ["hint", "a.selected"], ["nope", "a.selected"]]) {
    const hit = findBaseCloud(two, [], "m", ops, bundles, via)?.resolved;
    assert.equal(hit ? `${hit.nodeId}.${hit.port}` : null, want, `viaPort=${via}`);
  }
});

test("量测读数：节点底栏写四位有效数字 + 单位与判定，悬停有全文；运行收尾里有问题的在上", () => {
  const m = (port, value, extra = {}) => ({
    port, type: "Measurement", elementCount: 1, value: { kind: "Measurement", value, unit: "mm", ok: true, ...extra },
  });
  const outputs = [
    { port: "cloud", type: "PointCloud", elementCount: 1200 },
    m("gap", 3.78381, { verdict: "ok", nominal: 3.5, lower: 3, upper: 4 }),
    m("flush", 9.25, { verdict: "high" }),
    m("angle", null, { ok: false, message: "两侧交叉" }),
    m("raw", 0.12345),
  ];
  const got = readingsOf(outputs).map((r) => [r.port, r.text, r.verdict, r.tone]);
  assert.deepEqual(got, [
    ["gap", "3.784 mm", "ok", "ok"],
    ["flush", "9.25 mm", "high", "ng"],
    ["angle", "未测出", null, null],
    ["raw", "0.1235 mm", null, null],
  ]);
  assert.equal(readingsOf(outputs)[0].title, "gap = 3.78381 mm（ok）；标称 3.5，下限 3，上限 4", "悬停是六位有效数字与上下限");
  assert.deepEqual(readingsOf([{ port: "cloud", type: "PointCloud", elementCount: 5 }]), [], "没有量测输出：底栏照旧写元素数");
  assert.equal(formatOutputValue({ value: undefined }), "0 个元素", "运行收尾里的输出可能不带元素数");

  const summary = (state, verdict) => ({ state, node: "n", port: "p", value: verdict ? { kind: "Measurement", value: 1, verdict } : undefined });
  const order = sortSummaryOutputs([
    ["a", summary("value", "ok")],
    ["b", summary("value", "margin")],
    ["c", summary("inactive")],
    ["d", summary("value", "low")],
    ["e", summary("failed")],
    ["f", summary("value", "fail")],
    ["g", summary("value")],
  ]).map(([name]) => name);
  assert.deepEqual(order, ["d", "e", "f", "b", "a", "c", "g"], "崩了的与判不合格的在最上，其次接近边界，其余照原样");
});

