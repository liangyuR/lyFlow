// param-recipe P2.3–P2.5、P2.8：参数面板「按节点」页的数据模型（lib/paramPanel.ts）。
// 行的全集、各 chip 的判据、搜索、折叠默认值、子图定义的展开与共享计数、诊断挂到哪一行。
import assert from "node:assert/strict";
import { test } from "node:test";

import { buildPanelModel, flattenPanel, NO_DIAGNOSTICS } from "../src/lib/paramPanel.ts";
import { augmentOperators } from "../src/lib/subgraph.ts";

const OP = {
  id: "t.op",
  version: "1.0.0",
  label: "测试算子",
  category: "T",
  inputs: [],
  outputs: [],
  params: [
    { name: "mode", type: "enum", default: "a", group: "基础", options: [{ value: "a", label: "A" }, { value: "b", label: "B" }] },
    { name: "gain", type: "float", label: "Gain", default: 1, group: "基础" },
    { name: "extra", type: "int", default: 0, group: "基础", visibleWhen: { param: "mode", eq: "b" } },
    { name: "tol", type: "float", default: 0.1, group: "基础", enabledWhen: { param: "mode", ne: "a" } },
    { name: "seed", type: "int", default: 0, advanced: true },
    { name: "note", type: "string", default: "" },
  ],
};

const base = new Map([[OP.id, OP]]);

function doc() {
  return {
    schemaVersion: 1,
    id: "d",
    nodes: [
      { id: "a", op: "t.op", params: { gain: 2 }, ui: { title: "甲" } },
      { id: "b", op: "t.op", params: { mode: "b", note: "hello world" } },
      { id: "s1", op: "sub:g", params: {} },
      { id: "s2", op: "sub:g", params: {} },
    ],
    edges: [],
    subgraphs: {
      g: {
        name: "G",
        nodes: [{ id: "in1", op: "t.op", params: {} }],
        edges: [],
        inputs: [],
        outputs: [],
        params: [],
      },
    },
    params: { gain: { type: "float", default: 3, binds: ["b.gain"] } },
  };
}

function model(d = doc(), diagnostics = NO_DIAGNOSTICS) {
  return buildPanelModel({ doc: d, ops: augmentOperators(base, d.subgraphs), path: [], overrides: {}, diagnostics });
}

const view = (m, filter = {}) => flattenPanel(m, { query: "", chip: "all", type: null, toggled: {}, ...filter });

test("行的全集：图参数 + 每个节点的可见参数 + 按实例展开的子图定义", () => {
  const m = model();
  assert.equal(m.graphRows.length, 1);
  const [a, b, s1, s2] = m.sections;
  // a：mode、gain、tol、note，advanced 的 seed 排到最后（extra 的 visibleWhen 不满足，不算）
  assert.deepEqual(a.rows.map((r) => r.param.name), ["mode", "gain", "tol", "note", "seed"]);
  // b：mode = b，extra 露出来
  assert.deepEqual(b.rows.map((r) => r.param.name), ["mode", "gain", "extra", "tol", "note", "seed"]);
  assert.equal(s1.subgraphId, "g");
  assert.equal(s1.children.length, 1);
  assert.equal(s1.children[0].shared, 2, "定义被两个实例共享");
  assert.equal(s1.children[0].fullNodeId, "s1/in1");
  assert.equal(s2.children[0].fullNodeId, "s2/in1");
  // 1 + 5 + 6 + 5 + 5
  assert.equal(view(m).counts.all, 22);
});

test("chip：已改动 = 与算子默认不同；配方 = 图参数与被它提供的行", () => {
  const v = view(model());
  // a.gain=2、b.mode=b、b.gain=3（由图参数提供）、b.note、图参数 gain（3 ≠ 算子默认 1）
  assert.equal(v.counts.modified, 5);
  assert.equal(v.counts.recipe, 2);
  const rows = view(model(), { chip: "recipe" }).items.filter((i) => i.kind === "row");
  assert.deepEqual(rows.map((i) => i.key), ["b.gain"]);
  assert.equal(rows[0].row.binding.graphParam, "gain");
  assert.equal(rows[0].row.value, 3);
});

test("enabledWhen 不满足的置灰（ne），advanced 组默认收起、过滤时展开", () => {
  const m = model();
  const tolA = m.sections[0].rows.find((r) => r.param.name === "tol");
  const tolB = m.sections[1].rows.find((r) => r.param.name === "tol");
  assert.equal(tolA.enabled, false);
  assert.equal(tolB.enabled, true);
  const items = view(m).items;
  const adv = items.find((i) => i.kind === "group-head" && i.advanced && i.key.startsWith("n:a|"));
  assert.equal(adv.open, false);
  assert.ok(!items.some((i) => i.kind === "row" && i.key === "a.seed"), "收起的组里的行不挂");
  // 点开：toggled 里记一笔
  const opened = view(m, { toggled: { [adv.key]: true } }).items;
  assert.ok(opened.some((i) => i.kind === "row" && i.key === "a.seed"));
  // 搜索时一律展开
  const searched = view(m, { query: "seed" }).items.filter((i) => i.kind === "row");
  assert.deepEqual(searched.map((i) => i.key), ["a.seed", "b.seed", "s1/in1.seed", "s2/in1.seed"]);
});

test("子图定义默认收起；展开后定义里的节点带共享标记", () => {
  const m = model();
  const def = view(m).items.find((i) => i.kind === "def-head" && i.key === "n:s1|def");
  assert.equal(def.open, false);
  assert.equal(def.shared, 2);
  const open = view(m, { toggled: { "n:s1|def": true } }).items;
  const head = open.find((i) => i.kind === "node-head" && i.key === "n:s1/in1");
  assert.equal(head.section.shared, 2);
  assert.ok(open.some((i) => i.kind === "row" && i.key === "s1/in1.mode" && i.row.shared === 2));
});

test("搜索：参数名、label、节点标题、节点 id、值的文本，多个词都要中", () => {
  const m = model();
  const keys = (q) => view(m, { query: q }).items.filter((i) => i.kind === "row").map((i) => i.key);
  assert.deepEqual(keys("hello"), ["b.note"]);
  assert.deepEqual(keys("甲 gain"), ["a.gain"]);
  assert.deepEqual(keys("s2 mode"), ["s2/in1.mode"]);
  assert.equal(view(m, { query: "Gain" }).counts.all, 5, "大小写不敏感：4 个节点的 gain + 图参数 gain");
  assert.deepEqual(view(m, { query: "zzz" }).items.map((i) => i.kind), ["empty"]);
});

test("类型过滤与诊断：带 paramPath 的诊断贴到那一行，图参数的贴到图参数行", () => {
  const diagnostics = {
    byNode: new Map([["a", new Map([["gain", [{ severity: "error", message: "太大" }]]])]]),
    byGraphParam: new Map([["gain", [{ severity: "error", message: "越界" }]]]),
  };
  const m = model(doc(), diagnostics);
  const v = view(m, { chip: "diag" });
  assert.equal(v.counts.diag, 2);
  assert.deepEqual(
    v.items.filter((i) => i.kind === "row" || i.kind === "gp-row").map((i) => i.key),
    ["gp:gain", "a.gain"],
  );
  const ints = view(m, { type: "int" }).items.filter((i) => i.kind === "row").map((i) => i.key);
  assert.deepEqual(ints, ["a.seed", "b.extra", "b.seed", "s1/in1.seed", "s2/in1.seed"]);
  assert.equal(view(m).counts.byType.int, 5);
});
