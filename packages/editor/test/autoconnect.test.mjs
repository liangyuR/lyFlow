// m8-plan L13 / L14：按类型自动连线与带自动连线的片段插入。纯逻辑，不开浏览器 ——
// 画布上的手感（拖入、高亮）在 scripts/e2e/m8b.mjs 里走真实鼠标。
import assert from "node:assert/strict";
import { test } from "node:test";

import { healPlan, planAutoConnect } from "../src/lib/autoconnect.ts";
import { addNodeWithAutoConnect } from "../src/lib/insert.ts";
import { decodeNodeClipboard, encodeNodeClipboard } from "../src/lib/nodeClipboard.ts";
import { useGraphStore } from "../src/store/graph.ts";
import { useManifestStore } from "../src/store/manifest.ts";
import { useUiStore } from "../src/store/ui.ts";

const port = (name, type, extra = {}) => ({ name, type, ...extra });
const op = (id, inputs, outputs) => ({ id, version: "1.0.0", label: id, category: "t", inputs, outputs, params: [] });

// gap 积木算子的端口形状，缩到这几条测试要的那些
const bundle = {
  schemaVersion: 1,
  types: [
    { name: "PointCloud", color: "#1" },
    { name: "Box2D", color: "#2" },
    { name: "Line2D", color: "#3" },
    { name: "Measurement", color: "#4" },
    { name: "Any", color: "#5" },
    { name: "Bundle", color: "#6" },
  ],
  bundles: [
    { kind: "t.Scan", fields: [{ name: "merged", type: "PointCloud" }] },
    { kind: "t.Rois", fields: [{ name: "datum", type: "Box2D" }] },
  ],
  operators: [
    op("t.read", [port("primary", "PointCloud", { required: false })], [port("scan", "Bundle<t.Scan>")]),
    op("t.locate", [port("scan", "Bundle<t.Scan>")], [port("rois", "Bundle<t.Rois>"), port("scan", "Bundle<t.Scan>")]),
    op("t.line", [port("scan", "Bundle<t.Scan>"), port("rois", "Bundle<t.Rois>"), port("refLine", "Line2D", { required: false })],
      [port("line", "Line2D")]),
    op("t.flush", [port("baseLine", "Line2D")], [port("value", "Measurement")]),
    op("t.judge", [port("value", "Measurement")], [port("value", "Measurement")]),
    op("t.reroute", [port("in", "Any")], [port("out", "Any")]),
  ],
};

function reset() {
  useManifestStore.getState().replaceBundle(structuredClone(bundle), 0);
  useGraphStore.getState().newDoc();
  useUiStore.getState().clearAutoHint();
}

function ctx() {
  const m = useManifestStore.getState();
  return { operatorsById: m.operatorsById, typesByName: m.typesByName };
}

const doc = () => useGraphStore.getState().doc;
const edgesInto = (node) => doc().edges.filter((e) => e.to.node === node);

test("唯一候选自动连上；拖入节点只算一条撤销", () => {
  reset();
  const g = useGraphStore.getState();
  const read = g.addNode("t.read", { x: 0, y: 0 });
  const r = useGraphStore.getState().addNodeAuto("t.locate", { x: 200, y: 0 });
  assert.equal(r.wired, 1);
  assert.deepEqual(edgesInto(r.nodeIds[0]).map((e) => e.from), [{ node: read, port: "scan" }]);
  useGraphStore.getState().undo();
  assert.equal(doc().nodes.length, 1);
  assert.equal(doc().edges.length, 0);
});

test("两个候选时不连，候选端口进 autoHint", () => {
  reset();
  const g = useGraphStore.getState();
  const a = g.addNode("t.read", { x: 0, y: 0 });
  const b = useGraphStore.getState().addNode("t.read", { x: 0, y: 100 });
  const r = useGraphStore.getState().addNodeAuto("t.locate", { x: 200, y: 0 });
  assert.equal(r.wired, 0);
  assert.equal(r.ambiguous.length, 1);
  assert.deepEqual(new Set(r.ambiguous[0].candidates.map((c) => c.node)), new Set([a, b]));
  const hint = useUiStore.getState().autoHint;
  assert.ok(hint.targets.has(`${r.nodeIds[0]}:scan`));
  assert.ok(hint.candidates.has(`${a}:scan`) && hint.candidates.has(`${b}:scan`));
});

test("被下游同类型输出取代的那个输出不算候选：read.scan 接进 locate 之后只剩 locate.scan", () => {
  reset();
  const g = useGraphStore.getState();
  g.addNode("t.read", { x: 0, y: 0 });
  const locate = useGraphStore.getState().addNodeAuto("t.locate", { x: 200, y: 0 }).nodeIds[0];
  const r = useGraphStore.getState().addNodeAuto("t.line", { x: 400, y: 0 });
  assert.equal(r.wired, 2, JSON.stringify(r));
  for (const e of edgesInto(r.nodeIds[0])) assert.equal(e.from.node, locate);
  // 可选输入（refLine）不自动接
  assert.ok(!edgesInto(r.nodeIds[0]).some((e) => e.to.port === "refLine"));
});

test("类型还推不出来的 Any 输出不当候选；Any 输入不去猜", () => {
  reset();
  const g = useGraphStore.getState();
  g.addNode("t.reroute", { x: 0, y: 0 });
  const r = useGraphStore.getState().addNodeAuto("t.flush", { x: 200, y: 0 });
  assert.equal(r.wired, 0);
  assert.equal(r.ambiguous.length, 0);
  const plan = planAutoConnect(ctx(), doc(), [{ node: g.addNode("t.reroute", { x: 0, y: 99 }), port: "in" }]);
  assert.equal(plan.wires.length, 0);
});

test("插入片段：内部边照搬，对外输入按提示接到片段外唯一的候选上，片段自己的节点不当候选", () => {
  reset();
  const g = useGraphStore.getState();
  g.addNode("t.read", { x: 0, y: 0 });
  const locate = useGraphStore.getState().addNodeAuto("t.locate", { x: 200, y: 0 }).nodeIds[0];
  const snippet = {
    id: "t.skeleton",
    label: "骨架",
    nodes: [
      { id: "line", op: "t.line", ui: { position: { x: 0, y: 0 }, title: "基准线" } },
      { id: "flush", op: "t.flush", ui: { position: { x: 200, y: 0 } } },
      { id: "judge", op: "t.judge", ui: { position: { x: 400, y: 0 } } },
      { id: "ghost", op: "t.nope" },
    ],
    edges: [
      { from: { node: "line", port: "line" }, to: { node: "flush", port: "baseLine" } },
      { from: { node: "flush", port: "value" }, to: { node: "judge", port: "value" } },
    ],
    ports: { inputs: [{ node: "line", port: "scan" }, { node: "line", port: "rois" }] },
  };
  const before = doc().edges.length;
  const r = useGraphStore.getState().insertSnippet(snippet, { x: 500, y: 300 });
  assert.equal(r.nodeIds.length, 3);
  assert.deepEqual(r.missing, ["t.nope"]);
  assert.equal(r.wired, 2);
  assert.equal(doc().edges.length, before + 2 + 2);
  const line = doc().nodes.find((n) => n.op === "t.line");
  assert.equal(line.ui.title, "基准线");
  assert.deepEqual(line.ui.position, { x: 500, y: 300 });
  for (const e of edgesInto(line.id)) assert.equal(e.from.node, locate);
  // 一次插入一条撤销
  useGraphStore.getState().undo();
  assert.equal(doc().edges.length, before);
  assert.equal(doc().nodes.length, 2);
});

test("界面上加了节点就记进「最近用过」：挪到最前、不重复、最多 8 个；没加上不记", () => {
  reset();
  useUiStore.setState({ recentOps: [] });
  addNodeWithAutoConnect("t.read", { x: 0, y: 0 });
  addNodeWithAutoConnect("t.locate", { x: 200, y: 0 });
  addNodeWithAutoConnect("t.read", { x: 0, y: 200 });
  assert.deepEqual(useUiStore.getState().recentOps, ["t.read", "t.locate"]);
  addNodeWithAutoConnect("t.nope", { x: 0, y: 400 });
  assert.deepEqual(useUiStore.getState().recentOps, ["t.read", "t.locate"], "当前 core 里没有的算子加不上，不记");
  for (let i = 0; i < 10; i += 1) useUiStore.getState().noteOperatorUsed(`x.${i}`);
  assert.deepEqual(useUiStore.getState().recentOps, ["x.9", "x.8", "x.7", "x.6", "x.5", "x.4", "x.3", "x.2"]);
});

test("复制粘贴（P0 #12）：id 重映射、内部连线留着、整体平移到落点、静音照旧；系统剪贴板的两种文字都认", () => {
  reset();
  const r = addNodeWithAutoConnect("t.read", { x: 100, y: 100 }).nodeIds[0];
  const l = addNodeWithAutoConnect("t.locate", { x: 300, y: 140 }).nodeIds[0];
  useGraphStore.getState().setBypass([l], true);
  const doc = useGraphStore.getState().doc;
  const clip = {
    nodes: doc.nodes.filter((n) => [r, l].includes(n.id)),
    edges: doc.edges.filter((e) => e.from.node === r && e.to.node === l),
  };
  assert.equal(clip.edges.length, 1, "两个节点之间自动连上了一条");

  // 经系统剪贴板走一圈：编码、解码，与原样相同
  const round = decodeNodeClipboard(encodeNodeClipboard(clip));
  assert.deepEqual(round, clip);
  const pasted = useGraphStore.getState().pasteNodes(round, { x: 1000, y: 500 });
  assert.equal(pasted.nodeIds.length, 2);
  const after = useGraphStore.getState().doc;
  const [pr, pl] = pasted.nodeIds.map((id) => after.nodes.find((n) => n.id === id));
  assert.ok(![r, l].includes(pr.id) && ![r, l].includes(pl.id), "id 重新分配");
  assert.deepEqual([pr.ui.position, pl.ui.position], [{ x: 1000, y: 500 }, { x: 1200, y: 540 }], "左上角落到落点，相对位置不变");
  assert.equal(pl.bypass, true, "静音的粘出来还是静音的");
  assert.ok(after.edges.some((e) => e.from.node === pr.id && e.to.node === pl.id), "内部连线跟着走");

  // 整张图的 JSON 也认；外部连线（一端不在这批节点里）丢掉；别的文字不认
  const fromDoc = decodeNodeClipboard(JSON.stringify({ schemaVersion: 1, nodes: clip.nodes, edges: [...clip.edges,
    { id: "x", from: { node: "elsewhere", port: "scan" }, to: { node: l, port: "scan" } }] }));
  assert.deepEqual(fromDoc?.edges, clip.edges);
  for (const text of ["", "hello", "[1,2]", '{"nodes":[{"id":"a","op":"t.read"}]}', '{"kind":"lyflow.nodes","nodes":[]}']) {
    assert.equal(decodeNodeClipboard(text), null, text);
  }
});

test("删节点（P0 #5）：连着它的边一并删掉，别的边不动；一次撤销全回来", () => {
  reset();
  const a = addNodeWithAutoConnect("t.read", { x: 0, y: 0 }).nodeIds[0];
  const b = addNodeWithAutoConnect("t.locate", { x: 200, y: 0 }).nodeIds[0];
  const c = addNodeWithAutoConnect("t.line", { x: 400, y: 0 }).nodeIds[0];
  const edgesOf = () => useGraphStore.getState().doc.edges.map((e) => `${e.from.node}>${e.to.node}`).sort();
  const before = edgesOf();
  assert.ok(before.includes(`${a}>${b}`) && before.includes(`${b}>${c}`), JSON.stringify(before));
  useGraphStore.getState().deleteNodes([b]);
  assert.deepEqual(edgesOf().filter((e) => e.includes(b)), [], "连着 b 的边都没了");
  assert.equal(useGraphStore.getState().doc.nodes.some((n) => n.id === b), false);
  useGraphStore.getState().undo();
  assert.deepEqual(edgesOf(), before);
});

// Ctrl+Delete「删除并接通」：被删节点的每个输出取它第一个类型兼容、已连线的输入的来源（与静音透传同一条规则）
test("healPlan：删掉节点时上下游怎么接回去 —— 单个、一分二、合并取第一个输入、连着删一串、隔着 reroute、源头接不回", () => {
  const P = (name, type) => ({ name, type, label: name, doc: "", required: true });
  const O = (id, inputs, outputs) => ({ id, label: id, inputs, outputs, params: [] });
  const ctx = {
    operatorsById: new Map([
      ["gen", O("gen", [], [P("cloud", "PointCloud")])],
      ["vox", O("vox", [P("cloud", "PointCloud")], [P("cloud", "PointCloud")])],
      ["merge", O("merge", [P("a", "PointCloud"), P("b", "PointCloud")], [P("cloud", "PointCloud")])],
      ["reroute", O("reroute", [P("in", "Any")], [P("out", "Any")])],
    ]),
    typesByName: new Map([["PointCloud", { name: "PointCloud", color: "#1" }], ["Any", { name: "Any", color: "#2" }]]),
  };
  const E = (f, fp, t, tp) => ({ id: `${f}.${fp}-${t}.${tp}`, from: { node: f, port: fp }, to: { node: t, port: tp } });
  const N = (id, opId) => ({ id, op: opId, params: {} });
  const doc = {
    schemaVersion: 1, id: "h",
    nodes: [N("g", "gen"), N("g2", "gen"), N("v1", "vox"), N("v2", "vox"), N("v3", "vox"), N("m", "merge"), N("r", "reroute"), N("w", "vox")],
    edges: [E("g", "cloud", "v1", "cloud"), E("v1", "cloud", "v2", "cloud"), E("v2", "cloud", "v3", "cloud"),
            E("g", "cloud", "m", "a"), E("g2", "cloud", "m", "b"), E("m", "cloud", "w", "cloud")],
  };
  const fanOut = { ...doc, nodes: [...doc.nodes, N("v4", "vox")], edges: [...doc.edges, E("v1", "cloud", "v4", "cloud")] };
  const viaReroute = { ...doc, edges: [E("g", "cloud", "r", "in"), E("r", "out", "v2", "cloud")] };
  const wires = (plan) => plan.wires.map((w) => `${w.from.node}.${w.from.port}>${w.to.node}.${w.to.port}`).sort();
  const cases = [
    ["删 v1：g 接到 v2", doc, ["v1"], ["g.cloud>v2.cloud"], 0],
    ["v1 一分二：两支都接回 g", fanOut, ["v1"], ["g.cloud>v2.cloud", "g.cloud>v4.cloud"], 0],
    ["删合并：取第一个输入 a 的来源", doc, ["m"], ["g.cloud>w.cloud"], 0],
    ["连着删 v1、v2：g 接到 v3", doc, ["v1", "v2"], ["g.cloud>v3.cloud"], 0],
    ["隔着 reroute（Any 推成点云）", viaReroute, ["r"], ["g.cloud>v2.cloud"], 0],
    ["删源头 g：下游接不回", doc, ["g"], [], 2],
  ];
  for (const [name, d, ids, want, unresolved] of cases) {
    const plan = healPlan(ctx, d, new Set(ids));
    assert.deepEqual({ wires: wires(plan), unresolved: plan.unresolved }, { wires: want, unresolved }, name);
  }
});

test("deleteNodesHealing：删中间那个、上下游接回去，一条撤销", () => {
  reset();
  const g = useGraphStore.getState();
  const j1 = g.addNode("t.judge", { x: 0, y: 0 });
  const j2 = useGraphStore.getState().addNode("t.judge", { x: 200, y: 0 });
  const j3 = useGraphStore.getState().addNode("t.judge", { x: 400, y: 0 });
  useGraphStore.getState().connect({ node: j1, port: "value" }, { node: j2, port: "value" });
  useGraphStore.getState().connect({ node: j2, port: "value" }, { node: j3, port: "value" });
  const past = useGraphStore.getState().past.length;
  const edgesOf = () => useGraphStore.getState().doc.edges.map((e) => `${e.from.node}>${e.to.node}`).sort();
  assert.deepEqual(useGraphStore.getState().deleteNodesHealing([j2]), { wired: 1, unresolved: 0, dropped: [] });
  assert.deepEqual(edgesOf(), [`${j1}>${j3}`]);
  assert.equal(useGraphStore.getState().past.length - past, 1, "一条撤销");
  useGraphStore.getState().undo();
  assert.deepEqual(edgesOf(), [`${j1}>${j2}`, `${j2}>${j3}`].sort());
});

test("batch：一个手势里的几个动作一条撤销；拖动的事务里并进那一条；cancel 撤回、不记", () => {
  reset();
  const g = () => useGraphStore.getState();
  const a = addNodeWithAutoConnect("t.read", { x: 0, y: 0 }).nodeIds[0];
  const b = addNodeWithAutoConnect("t.locate", { x: 200, y: 0 }).nodeIds[0];
  const edge = doc().edges.find((e) => e.from.node === a && e.to.node === b).id;
  const start = { doc: doc(), past: g().past.length };

  // Delete 键删掉框选的节点与连线：断边、删节点以前记成两条
  g().batch("删除节点", () => {
    g().disconnect([edge]);
    g().deleteNodes([b]);
  });
  assert.equal(g().past.length, start.past + 1);
  assert.equal(g().past.at(-1).label, "删除节点");
  g().undo();
  assert.equal(doc(), start.doc, "一次撤销全回来");

  // 拖到连线上松手：插入并进拖动的那一条，由拖动的 commit 记
  g().begin();
  g().moveNodes([{ id: b, position: { x: 260, y: 40 } }]);
  g().batch("插入到连线中间", () => g().addNode("t.reroute", { x: 0, y: 100 }));
  assert.equal(g().past.length, start.past, "拖动没放手之前一条都不记");
  g().commit("插入到连线中间");
  assert.equal(g().past.length, start.past + 1);
  g().undo();
  assert.equal(doc(), start.doc);

  // cancel：fn 里做的全撤回，不记撤销，也不留着事务
  const id = g().batch("插入 Reroute", (cancel) => {
    const added = g().addNode("t.reroute", { x: 0, y: 0 });
    cancel();
    return added;
  });
  assert.ok(id, "返回 fn 的结果");
  assert.equal(doc(), start.doc);
  assert.equal(g().past.length, start.past);
  assert.equal(g().pendingSnapshot, null);

  // abort：拖 2D 框拖到一半按 Esc —— 回到开始拖的那一刻，不记撤销
  g().begin();
  g().moveNodes([{ id: a, position: { x: 999, y: 999 } }]);
  g().abort();
  assert.deepEqual([doc() === start.doc, g().past.length, g().pendingSnapshot], [true, start.past, null]);
});
