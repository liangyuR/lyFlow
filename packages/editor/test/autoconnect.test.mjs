// m8-plan L13 / L14：按类型自动连线与带自动连线的片段插入。纯逻辑，不开浏览器 ——
// 画布上的手感（拖入、高亮）在 scripts/e2e/m8b.mjs 里走真实鼠标。
import assert from "node:assert/strict";
import { test } from "node:test";

import { planAutoConnect } from "../src/lib/autoconnect.ts";
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
