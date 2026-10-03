// lib/typecheck：拖线时的即时挡错（交互清单 P0 #7 端口类型校验、#16 环检测、E6 Any 端口推导）。
// 这一层是「手感」：权威校验在 C++（docs/architecture.md），这里钉住的是给人看的判断与原因。
// 真鼠标拖线时的置灰与吸附在 e2e scripts/e2e/m3.mjs、m8b.mjs。
import assert from "node:assert/strict";
import { test } from "node:test";

import { createMappingCache, toReactFlow } from "../src/lib/mapping.ts";
import { canConnect, compatibleSources, compatibleTargets, dropOnNode, dropOnPort, inferAnyTypes, insertPortsFor, pendingPort, wouldCreateCycle } from "../src/lib/typecheck.ts";

const port = (name, type) => ({ name, type, label: name, doc: "", required: true });
const op = (id, inputs, outputs) => ({ id, label: id, inputs, outputs, params: [] });
const ctx = {
  operatorsById: new Map([
    ["gen", op("gen", [], [port("cloud", "PointCloud")])],
    ["genI", op("genI", [], [port("cloud", "PointCloudXYZI")])],
    ["voxel", op("voxel", [port("cloud", "PointCloud")], [port("cloud", "PointCloud")])],
    ["intensity", op("intensity", [port("cloud", "PointCloudXYZI")], [])],
    ["box", op("box", [port("box", "Box2D")], [])],
    ["reroute", op("reroute", [port("in", "Any")], [port("out", "Any")])],
  ]),
  typesByName: new Map([
    ["PointCloud", { name: "PointCloud", color: "#1" }],
    ["PointCloudXYZI", { name: "PointCloudXYZI", color: "#2", castableTo: ["PointCloud"] }],
    ["Box2D", { name: "Box2D", color: "#3" }],
    ["Any", { name: "Any", color: "#4" }],
  ]),
};
const node = (id, opId) => ({ id, op: opId, params: {} });
const edge = (from, fromPort, to, toPort) => ({
  id: `${from}.${fromPort}-${to}.${toPort}`,
  from: { node: from, port: fromPort },
  to: { node: to, port: toPort },
});

//   g → v1 → v2        gi（另一种点云）     r1 → r2（两级 reroute，Any）     b（Box2D）  i（要 XYZI）  ghost（没注册的算子）
const doc = {
  schemaVersion: 1,
  id: "t",
  nodes: [
    node("g", "gen"), node("gi", "genI"), node("v1", "voxel"), node("v2", "voxel"), node("v3", "voxel"),
    node("r1", "reroute"), node("r2", "reroute"), node("b", "box"), node("i", "intensity"), node("ghost", "nope"),
  ],
  edges: [edge("g", "cloud", "v1", "cloud"), edge("v1", "cloud", "v2", "cloud"), edge("g", "cloud", "r1", "in"), edge("r1", "out", "r2", "in")],
};

test("canConnect：每一种拒绝都给出人话原因，能连的放行", () => {
  const P = (n, p) => ({ node: n, port: p });
  const cases = [
    [P("g", "cloud"), P("g", "cloud"), "不能连到自己"],
    [P("g", "cloud"), P("nobody", "cloud"), "节点不存在"],
    [P("g", "cloud"), P("ghost", "cloud"), "算子未注册"],
    [P("g", "nope"), P("v3", "cloud"), "没有输出端口 nope"],
    [P("g", "cloud"), P("v3", "nope"), "没有输入端口 nope"],
    [P("g", "cloud"), P("b", "box"), "类型不匹配：PointCloud → Box2D"],
    // castableTo 是有方向的：XYZI 能当 PointCloud 用，反过来不行
    [P("gi", "cloud"), P("v3", "cloud"), null],
    [P("g", "cloud"), P("i", "cloud"), "类型不匹配：PointCloud → PointCloudXYZI"],
    // E6：隔着两级 reroute，r2 的 Any 已经推成 PointCloud，照样挡住
    [P("r2", "out"), P("b", "box"), "类型不匹配：PointCloud → Box2D"],
    [P("r2", "out"), P("v3", "cloud"), null],
    // 输入是单连接；重连同一对端口不算占用
    [P("gi", "cloud"), P("v1", "cloud"), "输入端口已有连线"],
    [P("g", "cloud"), P("v1", "cloud"), null],
    [P("v2", "cloud"), P("v3", "cloud"), null],
  ];
  for (const [from, to, reason] of cases) {
    const v = canConnect(ctx, doc, from, to);
    const label = `${from.node}.${from.port} → ${to.node}.${to.port}`;
    if (reason === null) assert.deepEqual(v, { ok: true }, label);
    else assert.ok(!v.ok && v.reason.includes(reason), `${label}: ${JSON.stringify(v)}`);
  }
  // 成环：r2 改由 v3 喂（去掉 r1 → r2），再从 r2 连回 v3 的输入 —— v3 的输入是空的、类型也接得住，只剩成环这一条理由
  const looped = { ...doc, edges: [...doc.edges, edge("v3", "cloud", "r2", "in")].filter((e) => e.to.node !== "r2" || e.from.node === "v3") };
  assert.equal(wouldCreateCycle(looped, "r2", "v3"), true);
  const v = canConnect(ctx, looped, { node: "r2", port: "out" }, { node: "v3", port: "cloud" });
  assert.deepEqual(v, { ok: false, reason: "会形成环" });
  assert.equal(wouldCreateCycle(doc, "v2", "v3"), false);
  assert.equal(wouldCreateCycle(doc, "v2", "g"), true, "g 在 v2 的上游：v2 → g 就成环");
});

test("inferAnyTypes：Any 沿连线往下游推，也从下游的具体输入往上游推；推不出来的不在表里", () => {
  const types = inferAnyTypes(ctx, doc);
  assert.equal(types.get("r1"), "PointCloud");
  assert.equal(types.get("r2"), "PointCloud", "两级之后照样推得到");
  // 只有下游连着具体类型的 reroute：反过来推
  const back = { ...doc, nodes: [...doc.nodes, node("r3", "reroute")], edges: [...doc.edges, edge("r3", "out", "b", "box")] };
  assert.equal(inferAnyTypes(ctx, back).get("r3"), "Box2D");
  const lonely = { ...doc, nodes: [...doc.nodes, node("r4", "reroute")] };
  assert.equal(inferAnyTypes(ctx, lonely).has("r4"), false);
});

test("compatibleTargets / compatibleSources：拖线时置灰与高亮的那张表", () => {
  // 从 g 拖：空着的 v3、本来就连着 g 的 v1 与 r1（重连同一对端口不算占用）；b、i 类型接不住，
  // v2、r2 已被别的源占着，ghost 没注册
  const targets = [...compatibleTargets(ctx, doc, { node: "g", port: "cloud" })].sort();
  assert.deepEqual(targets, ["r1:in", "v1:cloud", "v3:cloud"]);
  // 往 v3 的输入拖回去找源：点云类的输出都行（XYZI 可当 PointCloud 用；r2 推成了 PointCloud）
  const sources = [...compatibleSources(ctx, doc, { node: "v3", port: "cloud" })].sort();
  assert.deepEqual(sources, ["g:cloud", "gi:cloud", "r1:out", "r2:out", "v1:cloud", "v2:cloud"]);
});

test("dropOnNode：拖线松在节点的身子上 —— 恰好一个能接就接它，多个提示对准，接不上说为什么，松回自己不动", () => {
  // 加一个两路输入的 m（合并点云那种）
  const mctx = { ...ctx, operatorsById: new Map([...ctx.operatorsById, ["merge", op("merge", [port("a", "PointCloud"), port("b", "PointCloud")], [port("cloud", "PointCloud")])]]) };
  const mdoc = { ...doc, nodes: [...doc.nodes, node("m", "merge")] };
  const P = (n, p) => ({ node: n, port: p });
  const cases = [
    // [拖的那一头, 方向, 松在谁身上, 期望]
    [P("g", "cloud"), "output", "v3", { kind: "connect", from: P("g", "cloud"), to: P("v3", "cloud") }],
    [P("g", "cloud"), "output", "m", { kind: "reject", reason: "merge 上有 2 个端口能接，拖到要接的那个端口上" }],
    [P("g", "cloud"), "output", "b", { kind: "reject", reason: "接不到 box 上：类型不匹配：PointCloud → Box2D" }],
    [P("g", "cloud"), "output", "g", { kind: "self" }],
    [P("g", "cloud"), "output", "ghost", { kind: "none" }],
    // 反着拖：空着的输入找源（XYZI 能当 PointCloud 用）；已经接着线的输入说清楚是单连接
    [P("v3", "cloud"), "input", "gi", { kind: "connect", from: P("gi", "cloud"), to: P("v3", "cloud") }],
    [P("v1", "cloud"), "input", "gi", { kind: "reject", reason: "这个输入端口已有连线（输入是单连接）：拖它的线头才是改接" }],
    [P("v3", "cloud"), "input", "b", { kind: "reject", reason: "box 没有输出端口" }],
  ];
  for (const [ref, side, target, want] of cases) {
    assert.deepEqual(dropOnNode(mctx, mdoc, ref, side, target), want, `${ref.node}.${ref.port}（${side}）→ ${target}`);
  }
});

test("insertPortsFor：插到 g → v1 那条线中间 —— 恰好一对端口两头都接得上才给，不止一对或接不上给 null", () => {
  const ictx = {
    ...ctx,
    operatorsById: new Map([
      ...ctx.operatorsById,
      ["merge", op("merge", [port("a", "PointCloud"), port("b", "PointCloud")], [port("cloud", "PointCloud")])],
      ["pass", op("pass", [port("cloud", "PointCloud")], [port("cloud", "PointCloud"), port("idx", "Indices")])],
    ]),
    typesByName: new Map([...ctx.typesByName, ["Indices", { name: "Indices", color: "#5" }]]),
  };
  const idoc = { ...doc, nodes: [...doc.nodes, node("m", "merge"), node("p", "pass"), node("r3", "reroute")] };
  const line = doc.edges[0]; // g.cloud → v1.cloud
  const cases = [
    ["一进一出的体素", "v3", { inPort: "cloud", outPort: "cloud" }],
    ["两个输出只有一个接得上下游（直通滤波那种）", "p", { inPort: "cloud", outPort: "cloud" }],
    ["reroute（Any）", "r3", { inPort: "in", outPort: "out" }],
    ["两路输入都接得上（合并）：没有唯一解", "m", null],
    ["类型接不上", "b", null],
    ["没注册的算子", "ghost", null],
  ];
  for (const [name, nodeId, want] of cases) {
    assert.deepEqual(insertPortsFor(ictx, idoc, line, nodeId), want, name);
  }
});

test("dropOnPort：拖线松在一个具体端口上 —— 只判它；被占、类型不对、同一侧都说自己的原因，不另找", () => {
  const P = (n, p) => ({ node: n, port: p });
  const cases = [
    // [拖的那一头, 方向, 松手处的端口, 它的哪一侧, 期望]
    [P("g", "cloud"), "output", P("v3", "cloud"), "input", { kind: "connect", from: P("g", "cloud"), to: P("v3", "cloud") }],
    [P("g", "cloud"), "output", P("v2", "cloud"), "input", { kind: "reject", reason: "输入端口已有连线（输入是单连接）" }],
    [P("g", "cloud"), "output", P("b", "box"), "input", { kind: "reject", reason: "类型不匹配：PointCloud → Box2D" }],
    [P("g", "cloud"), "output", P("v1", "cloud"), "output", { kind: "reject", reason: "这是输出端口：拖到输入端口上" }],
    [P("g", "cloud"), "output", P("g", "cloud"), "output", { kind: "self" }],
    [P("v3", "cloud"), "input", P("gi", "cloud"), "output", { kind: "connect", from: P("gi", "cloud"), to: P("v3", "cloud") }],
  ];
  for (const [ref, side, target, targetSide, want] of cases) {
    assert.deepEqual(dropOnPort(ctx, doc, ref, side, target, targetSide), want, `${ref.node}.${ref.port} → ${target.node}.${target.port}（${targetSide}）`);
  }
});

test("pendingPort：拖线松在空白处挑了新算子，接它的哪个端口 —— 类型相同 > 能转 > Any，同分按必填与声明顺序", () => {
  const pctx = {
    ...ctx,
    operatorsById: new Map([
      ...ctx.operatorsById,
      ["merge", op("merge", [port("a", "PointCloud"), port("b", "PointCloud")], [port("cloud", "PointCloud")])],
      ["extract", op("extract", [port("cloud", "PointCloud"), port("indices", "Indices")], [port("selected", "PointCloud")])],
      ["ransac", op("ransac", [port("cloud", "PointCloud")], [port("inliers", "Indices"), port("plane", "Plane")])],
    ]),
    typesByName: new Map([...ctx.typesByName, ["Indices", { name: "Indices", color: "#5" }], ["Plane", { name: "Plane", color: "#6" }]]),
  };
  const pdoc = { ...doc, nodes: [...doc.nodes, node("ra", "ransac"), node("x", "extract"), node("r9", "reroute")] };
  const P = (n, p) => ({ node: n, port: p });
  const O = (id) => pctx.operatorsById.get(id);
  const cases = [
    // [拖出的那一头, 哪一侧, 新算子, 期望]
    ["点云 → 提取索引：cloud", P("g", "cloud"), "output", "extract", "cloud"],
    ["Indices → 提取索引：indices（修前固定接第一个 cloud，类型不对）", P("ra", "inliers"), "output", "extract", "indices"],
    ["点云 → 合并：a", P("g", "cloud"), "output", "merge", "a"],
    ["XYZI → 体素：经 castableTo", P("gi", "cloud"), "output", "voxel", "cloud"],
    ["点云 → Box2D 的算子：接不上", P("g", "cloud"), "output", "box", null],
    ["反着拖 提取索引.indices：接 RANSAC 的 inliers", P("x", "indices"), "input", "ransac", "inliers"],
    ["反着拖 提取索引.cloud：RANSAC 没有点云输出", P("x", "cloud"), "input", "ransac", null],
    ["推出了类型的 reroute（点云）→ 提取索引：cloud", P("r2", "out"), "output", "extract", "cloud"],
    ["没推出类型的 reroute：取第一个", P("r9", "out"), "output", "extract", "cloud"],
  ];
  for (const [name, from, side, opId, want] of cases) {
    assert.equal(pendingPort(pctx, pdoc, from, side, O(opId)), want, name);
  }
});

// 映射层（lib/mapping）与上面共用这一个夹具：连线颜色、虚线、Any 类型都是从类型推导来的
test("toReactFlow：连线按源端口的实际类型着色（隔着 reroute 也对）、惰性输入画虚线、节点带推导出的 Any 类型；没变的对象复用", () => {
  const lazyCtx = {
    ...ctx,
    operatorsById: new Map([...ctx.operatorsById, ["fallback", op("fallback", [{ ...port("alt", "PointCloud"), lazy: true }], [])]]),
  };
  const withLazy = { ...doc, nodes: [...doc.nodes, node("f", "fallback")], edges: [...doc.edges, edge("v2", "cloud", "f", "alt")] };
  const cache = createMappingCache();
  const { nodes, edges } = toReactFlow(withLazy, lazyCtx, undefined, undefined, undefined, cache);
  const byId = Object.fromEntries(edges.map((e) => [e.id, e]));
  assert.equal(byId["r1.out-r2.in"].style.stroke, "#1", "reroute 的输出推成了 PointCloud，颜色跟着源走");
  assert.equal(byId["g.cloud-v1.cloud"].style.stroke, "#1");
  assert.equal(byId["v2.cloud-f.alt"].data.lazy, true);
  assert.equal(byId["v2.cloud-f.alt"].style.strokeDasharray, "6 4");
  assert.equal(byId["g.cloud-v1.cloud"].data.lazy, false);
  assert.equal(nodes.find((n) => n.id === "r2").data.anyType, "PointCloud");
  assert.equal(nodes.find((n) => n.id === "ghost").data.anyType, null);
  // 同样的输入再映射一次：节点与连线数组整个复用（React Flow 靠引用不变走快路径）
  const again = toReactFlow(withLazy, lazyCtx, undefined, undefined, undefined, cache);
  assert.equal(again.nodes, nodes);
  assert.equal(again.edges, edges);
  // 只挪了一个节点的位置：它是新对象（位置变了），data 沿用原来那个 —— 节点组件按引用比 data，
  // 全选拖几百个节点时不该每帧整个重渲；没挪的节点整个复用
  const moved = { ...withLazy, nodes: withLazy.nodes.map((n) => (n.id === "v1" ? { ...n, ui: { position: { x: 999, y: 1 } } } : n)) };
  const after = toReactFlow(moved, lazyCtx, undefined, undefined, undefined, cache);
  const was = (id) => nodes.find((n) => n.id === id);
  const now = (id) => after.nodes.find((n) => n.id === id);
  assert.notEqual(now("v1"), was("v1"));
  assert.equal(now("v1").position.x, 999);
  assert.equal(now("v1").data, was("v1").data);
  assert.equal(now("g"), was("g"));
});
