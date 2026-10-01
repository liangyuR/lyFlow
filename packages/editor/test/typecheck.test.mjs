// lib/typecheck：拖线时的即时挡错（交互清单 P0 #7 端口类型校验、#16 环检测、E6 Any 端口推导）。
// 这一层是「手感」：权威校验在 C++（docs/architecture.md），这里钉住的是给人看的判断与原因。
// 真鼠标拖线时的置灰与吸附在 e2e scripts/e2e/m3.mjs、m8b.mjs。
import assert from "node:assert/strict";
import { test } from "node:test";

import { canConnect, compatibleSources, compatibleTargets, inferAnyTypes, wouldCreateCycle } from "../src/lib/typecheck.ts";

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
