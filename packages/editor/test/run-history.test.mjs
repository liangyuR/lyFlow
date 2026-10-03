// 调参记录（lib/runHistory、store/runHistory）：两次运行之间改了什么、读数比上一次变了多少、记哪几种运行。
// 抽屉「调参」页的真界面在 e2e m4.mjs 的 suitePreview 末尾。
import assert from "node:assert/strict";
import { test } from "node:test";

import { diffRuns, diffText, previousReading, runReadingsOf, shortValue } from "../src/lib/runHistory.ts";
import { MAX_RUN_RECORDS, useRunHistoryStore } from "../src/store/runHistory.ts";

const ops = new Map([
  ["filter.voxel_grid", { id: "filter.voxel_grid", label: "体素降采样", params: [
    { name: "leafSize", label: "Leaf Size", type: "vec3f", default: [0.01, 0.01, 0.01] },
    { name: "minPoints", label: "", type: "int", default: 1 },
  ] }],
  ["gen.synthetic", { id: "gen.synthetic", label: "合成点云", params: [{ name: "pointCount", label: "Point Count", type: "int", default: 30000 }] }],
]);

const base = {
  schemaVersion: 1,
  id: "d",
  nodes: [
    { id: "g", op: "gen.synthetic", params: {} },
    { id: "v", op: "filter.voxel_grid", params: { leafSize: [0.02, 0.02, 0.02] }, ui: { title: "粗降采样" } },
  ],
  edges: [],
  params: { thr: { type: "float", default: 1, label: "阈值" } },
};
const withNodes = (nodes, extra = {}) => ({ ...base, nodes, ...extra });

test("调参记录：两次运行之间改了什么 —— 有效参数（缺省值也算）、静音、增删节点、图参数；写成一行", () => {
  const rec = (doc, params = { thr: 1 }) => ({ doc, params });
  const cases = [
    // [说明, 后一次, 期望的一行]
    ["没改", rec(base), "参数没变"],
    ["稀疏存储里写上缺省值不算改", rec(withNodes([{ ...base.nodes[0], params: { pointCount: 30000 } }, base.nodes[1]])), "参数没变"],
    ["改一个参数", rec(withNodes([base.nodes[0], { ...base.nodes[1], params: { leafSize: [0.03, 0.03, 0.03] } }])),
      "粗降采样 · Leaf Size [0.02, 0.02, 0.02] → [0.03, 0.03, 0.03]"],
    ["参数没有 label 用名字", rec(withNodes([base.nodes[0], { ...base.nodes[1], params: { ...base.nodes[1].params, minPoints: 3 } }])),
      "粗降采样 · minPoints 1 → 3"],
    ["静音", rec(withNodes([{ ...base.nodes[0], bypass: true }, base.nodes[1]])), "静音 合成点云"],
    ["删一个、加一个", rec(withNodes([base.nodes[1], { id: "g2", op: "gen.synthetic", params: {} }])), "加了 合成点云；删了 合成点云"],
    ["图参数（配方叠上去之后的取值）", rec(base, { thr: 2.5 }), "图参数 · 阈值 1 → 2.5"],
    ["多了写「等 N 处」", rec(withNodes([{ ...base.nodes[0], params: { pointCount: 9 }, bypass: true }, { ...base.nodes[1], params: { leafSize: [1, 1, 1], minPoints: 2 } }]), { thr: 3 }),
      "合成点云 · Point Count 30000 → 9；粗降采样 · Leaf Size [0.02, 0.02, 0.02] → [1, 1, 1]；粗降采样 · minPoints 1 → 2 等 5 处"],
  ];
  for (const [name, next, want] of cases) assert.equal(diffText(diffRuns(rec(base), next, ops)), want, name);
  assert.equal(shortValue("x".repeat(40)).length, 24, "长字符串截短");
  assert.equal(shortValue(true), "开");
});

test("调参记录：读数按 id、端口排好，比上一次找同一个；预览与单节点运行不记，最多留 50 次", () => {
  const m = (port, value, verdict) => ({ port, type: "Measurement", elementCount: 1, value: { kind: "Measurement", value, unit: "mm", verdict } });
  const nodes = new Map([
    ["s/b", { state: "done", errors: [], stats: { outputs: [m("gap", 3.5, "ok")] } }],
    ["a", { state: "done", errors: [], stats: { outputs: [m("flush", Number.NaN, ""), { port: "cloud", type: "PointCloud", elementCount: 10 }] } }],
    ["c", { state: "error", errors: [] }],
  ]);
  const readings = runReadingsOf(nodes);
  assert.deepEqual(readings, [
    { id: "a", port: "flush", value: null, unit: "mm", verdict: null },
    { id: "s/b", port: "gap", value: 3.5, unit: "mm", verdict: "ok" },
  ]);
  assert.equal(previousReading({ readings }, { id: "s/b", port: "gap" })?.value, 3.5);
  assert.equal(previousReading(undefined, readings[0]), null);

  const h = () => useRunHistoryStore.getState();
  h().clear();
  h().begin("r-preview", base, {}, null, { preview: true });
  h().begin("r-iso", base, {}, null, { isolate: ["v"] });
  assert.equal(h().records.length, 0, "预览、单节点运行不记");
  h().begin("r1", base, { thr: 1 }, "夜班", { targets: ["v"] });
  h().finish("r1", "ok", 120, nodes);
  h().finish("gone", "ok", 1, nodes);
  assert.deepEqual(h().records.map((r) => [r.seq, r.status, r.durationMs, r.recipe, r.targets, r.readings.length]),
    [[1, "ok", 120, "夜班", ["v"], 2]]);
  // 运行到某个节点：节点表里留着上一次别的节点，只记这一次计划里的；被下一次顶掉的记成取消
  h().begin("r2", base, {}, null, { targets: ["s/b"] });
  h().planned("r2", ["s/b"]);
  h().begin("r3", base, {}, null, {});
  h().finish("r2", "ok", 5, nodes);
  assert.deepEqual(h().records.slice(0, 2).map((r) => [r.runId, r.status, r.readings.map((x) => x.id)]),
    [["r3", "running", []], ["r2", "ok", ["s/b"]]], "r2 的收场后到也照样记上；只记计划里的 s/b");
  h().begin("r4", base, {}, null, {});
  assert.equal(h().records[1].status, "cancelled", "r3 一直没收场就被 r4 顶掉：记成取消");
  for (let i = 0; i < MAX_RUN_RECORDS + 5; i += 1) h().begin(`x${i}`, base, {}, null, {});
  assert.equal(h().records.length, MAX_RUN_RECORDS, "只留最近 50 次");
  assert.equal(h().records[0].seq, 4 + MAX_RUN_RECORDS + 5, "新的在前，序号接着数（r1–r4 之后又 55 次）");
  h().clear();
  assert.deepEqual([h().records.length, h().seq], [0, 0], "换一张图清空、序号从头数");
});
