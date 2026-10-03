// lib/nodeRun 的图结构部分：智能运行的上游闭包（ancestorsOf）与右键「选中上游 / 下游」（closureOf）。
// 按钮本身的手感、预告与抢占在 e2e scripts/e2e/noderun.mjs 里走真界面。
import assert from "node:assert/strict";
import { test } from "node:test";

import { ancestorsOf, closureOf, previewTargets, stepAlong, upstreamToRun, willCompute } from "../src/lib/nodeRun.ts";

//   a → b → d → e
//        ↘     ↗
//   c ───→ x
//   f（孤立）
const edge = (from, to) => ({ id: `${from}-${to}`, from: { node: from, port: "out" }, to: { node: to, port: "in" } });
const level = {
  nodes: ["a", "b", "c", "d", "e", "f", "x"].map((id) => ({ id, op: "t.op" })),
  edges: [edge("a", "b"), edge("b", "d"), edge("d", "e"), edge("b", "x"), edge("c", "x"), edge("x", "e")],
};

test("closureOf：沿连线往上 / 往下走到头，含起点，按文档顺序；多个起点合在一起。拖参数时跟着重算的（previewTargets）只带下游的；拖图参数算它绑着的", async () => {
  const cases = [
    // [起点, 方向, 期望]
    [["d"], "up", ["a", "b", "d"]],
    [["d"], "down", ["d", "e"]],
    [["x"], "up", ["a", "b", "c", "x"]],
    [["b"], "down", ["b", "d", "e", "x"]],
    [["a", "c"], "down", ["a", "b", "c", "d", "e", "x"]],
    [["f"], "up", ["f"]],
    [["gone"], "down", []],
  ];
  for (const [start, dir, want] of cases) {
    assert.deepEqual(closureOf(level, start, dir), want, `${start.join("+")} ${dir}`);
  }
  // 拖 b 的参数：钉住的 / 对比 B 在 b 下游才一起算；上游、旁支、没有、就是 b 自己、重复的都不带
  const watched = [
    // [改的节点, 正看着的（钉住、对比 B）, 期望]
    ["b", ["e", null], ["b", "e"]],
    ["b", ["x", "d"], ["b", "x", "d"]],
    ["b", ["a", "c"], ["b"]],
    ["b", ["b", "e", "e"], ["b", "e"]],
    ["d", [null, null], ["d"]],
    ["c", ["e", "d"], ["c", "e"]],
  ];
  const here = (id) => (id ? { path: [], nodeId: id } : null);
  for (const [edited, eyes, want] of watched) {
    assert.deepEqual(previewTargets(level, [], edited, eyes.map(here)), want, `previewTargets ${edited} ← ${eyes.join(",")}`);
  }
  // 改的在子图里（参数面板展开的定义行是「S/p」，或者进了 S 改 p）、看着的在外面一层：从包着改动的 S 往下游找
  //   g → S{ p → q } → m      z（孤立）
  const sdoc = {
    nodes: ["g", "S", "m", "z"].map((id) => ({ id, op: id === "S" ? "sub:s" : "t.op" })),
    edges: [edge("g", "S"), edge("S", "m")],
    subgraphs: { s: { nodes: [{ id: "p", op: "t.op" }, { id: "q", op: "t.op" }], edges: [edge("p", "q")], inputs: [], outputs: [], params: [] } },
  };
  const inS = [{ nodeId: "S", subgraphId: "s" }];
  const nested = [
    // [说明, 当前层, 改的节点, 看着的, 期望]
    ["顶层改定义行，看着顶层的 m", [], "S/p", [{ path: [], nodeId: "m" }], ["S/p", "m"]],
    ["进了 S 改 p，B 在顶层的 m", inS, "p", [{ path: [], nodeId: "m" }], ["S/p", "m"]],
    ["顶层孤立的 z 不带", inS, "p", [{ path: [], nodeId: "z" }], ["S/p"]],
    ["同一层下游的 q", inS, "p", [{ path: inS, nodeId: "q" }], ["S/p", "S/q"]],
    ["看着的就是包着它的 S", inS, "p", [{ path: [], nodeId: "S" }], ["S/p", "S"]],
    ["看着的就是它自己", inS, "p", [{ path: inS, nodeId: "p" }], ["S/p"]],
    ["看着的在更深的别处：不认", [], "m", [{ path: inS, nodeId: "q" }], ["m"]],
  ];
  for (const [name, path, edited, eyes, want] of nested) {
    assert.deepEqual(previewTargets(sdoc, path, edited, eyes), want, `previewTargets：${name}`);
  }
  // 拖的是图参数：它绑着的每一个顶层节点（连同下游正看着的）合在一起；进了子图也按顶层算；没绑的什么都不跑
  const gdoc = { ...sdoc, params: { t: { binds: ["g.x", "S.y"] }, idle: { binds: [] } } };
  const { graphParamPreviewId, previewTargetsOf } = await import("../src/lib/preview.ts");
  assert.deepEqual(previewTargetsOf(gdoc, [], graphParamPreviewId("t"), [{ path: [], nodeId: "m" }]), ["g", "m", "S"]);
  assert.deepEqual(previewTargetsOf(gdoc, inS, graphParamPreviewId("t"), []), ["g", "S"], "当前层在子图里也按顶层节点算");
  assert.deepEqual(previewTargetsOf(gdoc, [], graphParamPreviewId("idle"), []), []);
  assert.deepEqual(previewTargetsOf(gdoc, inS, "p", []), ["S/p"], "节点参数照旧");
});

test("stepAlong（Alt+方向键沿连线走）：上下游取最上面的；刚从那边过来先退回去；同级在同一组里挪、到头不绕回", () => {
  // 同一张图摆上位置：b 的下游 d 在上、x 在下；x 的上游 b 在上、c 在下；e 的上游 d 在上、x 在下
  const pos = { a: [0, 0], b: [200, 0], c: [0, 200], d: [400, 0], x: [400, 200], e: [600, 100], f: [0, 400] };
  const placed = { ...level, nodes: level.nodes.map((n) => ({ ...n, ui: { position: { x: pos[n.id][0], y: pos[n.id][1] } } })) };
  const hop = (from, via, to) => ({ from, via, to });
  const cases = [
    // [说明, 当前, 方向, 上一步, 期望走到]
    ["下游：多个取最上面的", "b", "down", null, "d"],
    ["上游：多个取最上面的", "e", "up", null, "d"],
    ["刚从 c 往下走到 x：往上先退回 c（不是最上面的 b）", "x", "up", hop("c", "down", "x"), "c"],
    ["同级：b 的下游里 d 的下一个", "d", "next", null, "x"],
    ["同级：从 e 往上走到的 x，上一个是 d", "x", "prev", hop("e", "up", "x"), "d"],
    ["没有上游：在同一个下游的那几个上游里挪", "c", "prev", null, "b"],
    ["到头不绕回", "x", "next", null, null],
    ["源头往上", "a", "up", null, null],
    ["孤立节点往下", "f", "down", null, null],
    ["不在这一层", "gone", "up", null, null],
  ];
  for (const [name, cur, dir, last, want] of cases) {
    assert.equal(stepAlong(placed, cur, dir, last)?.id ?? null, want, name);
  }
  assert.deepEqual(stepAlong(placed, "x", "up", hop("c", "down", "x")).hop, hop("x", "up", "c"), "记下这一步，再往下能回到 x");
});

test("ancestorsOf：不含自己，离源头近的排前面", () => {
  // 从 e 往上数最长的那条路：a 三步，b、c 两步，d、x 一步
  assert.deepEqual(ancestorsOf(level, "e"), ["a", "b", "c", "d", "x"]);
  assert.deepEqual(ancestorsOf(level, "a"), []);
});

/** 原来的 ancestorsOf：每往上一层把全部连线扫一遍。缓存与索引版必须逐个给出同样的结果与顺序。 */
function ancestorsByScan(lvl, nodeId) {
  const depth = new Map();
  let frontier = [nodeId];
  for (let d = 1; frontier.length > 0 && d <= lvl.nodes.length; d += 1) {
    const next = [];
    for (const id of frontier) {
      for (const e of lvl.edges) {
        if (e.to.node !== id || e.from.node === nodeId) continue;
        const seen = depth.get(e.from.node);
        if (seen !== undefined && seen >= d) continue;
        depth.set(e.from.node, d);
        next.push(e.from.node);
      }
    }
    frontier = next;
  }
  return [...depth.entries()].sort((a, b) => b[1] - a[1]).map(([id]) => id);
}

test("ancestorsOf 的索引与缓存：与逐条扫的结果逐个相同；连线变了（新数组）就重算", () => {
  // 一张有扇出、有汇合的 60 节点图，边的先后打乱（同深度的先后靠它定）
  const nodes = Array.from({ length: 60 }, (_, i) => ({ id: `n${i}`, op: "t.op" }));
  const edges = [];
  for (let i = 1; i < 60; i += 1) {
    edges.push(edge(`n${Math.floor((i - 1) / 2)}`, `n${i}`));
    if (i % 3 === 0) edges.push(edge(`n${i - 2}`, `n${i}`));
  }
  edges.reverse();
  const lvl = { nodes, edges };
  for (const n of nodes) assert.deepEqual(ancestorsOf(lvl, n.id), ancestorsByScan(lvl, n.id), n.id);
  assert.equal(ancestorsOf(lvl, "n59"), ancestorsOf(lvl, "n59"), "同一份连线：直接给缓存");
  const changed = { nodes, edges: [...edges, edge("n1", "n58")] };
  assert.deepEqual(ancestorsOf(changed, "n59"), ancestorsByScan(changed, "n59"));
  assert.deepEqual(ancestorsOf(changed, "n58"), ancestorsByScan(changed, "n58"));
});

test("willCompute / upstreamToRun 的计划索引：与逐条扫计划的判断相同，子图节点看它的全部内部节点", () => {
  const plan = new Map([
    ["a", { nodeId: "a", cached: true }],
    ["b", { nodeId: "b", cached: false }],
    ["s/x", { nodeId: "s/x", cached: true }],
    ["s/y", { nodeId: "s/y", cached: false }],
    ["t/x", { nodeId: "t/x", cached: true }],
    ["lz", { nodeId: "lz", cached: false, lazy: true }],
  ]);
  const input = { path: [], plan, planUsable: true, execs: new Map(), stale: new Set() };
  assert.deepEqual(
    ["a", "b", "s", "t", "lz"].map((id) => willCompute(input, id)),
    [false, true, true, false, true],
    "惰性的条目不算（落回看有没有结果：没跑过就是要算）",
  );
  assert.deepEqual(upstreamToRun(input, ["a", "s", "t", "b"]), ["s", "b"]);
  // 换一层看同一份计划：s 里面的 x、y 是本层节点
  const inS = { ...input, path: [{ nodeId: "s", subgraphId: "S" }] };
  assert.deepEqual([willCompute(inS, "x"), willCompute(inS, "y")], [false, true]);
});
