// m7-plan J7/J8、§1 验收 7：顶层图参数 `params` 读写往返不丢字段。P1 起编辑器会经图参数动作改它
// （graph-params-actions.test.mjs），但不碰图参数的编辑一律不许动它：
// 打开（loadDoc）→ 做几步普通编辑 → 取出要存盘的 doc，`params` 必须逐字不变
// （含键顺序，所以比的是 JSON 文本而不只是深相等）。
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";

import { useGraphStore } from "../src/store/graph.ts";
import { useManifestStore } from "../src/store/manifest.ts";

const root = new URL("../../../", import.meta.url);
const readJson = (rel) => JSON.parse(readFileSync(new URL(rel, root), "utf8"));

const manifest = readJson("schema/examples/manifest.example.json");
const example = readJson("schema/examples/graph.example.lyflow.json");

/** 在样例图上再加一个多目标、带嵌套默认值的参数，覆盖「不丢字段」与键顺序。 */
function fixture() {
  const doc = structuredClone(example);
  doc.params = {
    ...doc.params,
    leaf: {
      default: [0.004, 0.004, 0.004],
      binds: ["n_voxel.leafSize", "n_clean.leafSize"],
      type: "vec3f",
      doc: "键顺序故意和 schema 里写的不一样",
    },
  };
  // 被绑定的参数不能在节点里再写值（J7 ②），样例里原先写着的要拿掉
  for (const id of ["n_voxel", "n_clean"]) delete doc.nodes.find((n) => n.id === id).params.leafSize;
  return doc;
}

/** 存盘时交给传输层的就是 store 里的 doc 本身；过一遍 JSON 等于落盘再读回。 */
function saved() {
  return JSON.parse(JSON.stringify(useGraphStore.getState().doc));
}

test("打开即保存：params 原样保留", () => {
  assert.ok(example.params && Object.keys(example.params).length > 0, "前提：样例图带顶层 params");
  const doc = fixture();
  assert.equal(Object.keys(doc.params).length, 2, "前提：fixture 里有两个图参数");
  const before = JSON.stringify(doc.params);
  useGraphStore.getState().loadDoc(structuredClone(doc), "g.lyflow.json");
  const out = saved();
  assert.deepEqual(out.params, doc.params);
  assert.equal(JSON.stringify(out.params), before);
  assert.deepEqual(out, doc);
});

/** 两份样例各走一串不碰图参数的编辑；`edit` 里顺带确认编辑确实生效，`check` 是该样例特有的收尾检查。 */
const EDIT_SAMPLES = [
  {
    name: "样例图 + 多目标参数",
    doc: fixture,
    edit(g) {
      g.setName("改个名");
      g.setParam("n_plane", "maxIterations", 500);
      g.moveNodes([{ id: "n_load", position: { x: 10, y: 20 } }]);
      g.setBypass(["n_voxel"], true);
      g.markGraphOutput({ node: "n_plane", port: "inliers" }, "floor");
      g.applyMigrations([
        {
          kind: "migration",
          nodeId: "n_load",
          op: "io.load_pcd",
          opVersion: "1.0.0",
          params: { path: "samples/other.pcd" },
        },
      ]);
      const edited = saved();
      assert.equal(edited.name, "改个名", "编辑确实生效了");
      assert.equal(edited.nodes.find((n) => n.id === "n_plane").params.maxIterations, 500);
    },
    undos: 2,
  },
  {
    name: "带完整规格的图参数（param-recipe P1.1）",
    doc: () => readJson("schema/examples/graph-params.example.lyflow.json"),
    edit(g) {
      g.setName("改个名");
      g.moveNodes([{ id: "n_gen", position: { x: 5, y: 5 } }]);
      g.setBypass(["n_cut"], true);
    },
    undos: 1,
    check(params) {
      // 规格字段逐个在（与 schema 样例同一份）
      for (const k of ["label", "doc", "group", "min", "max", "softMin", "softMax", "step", "unit", "componentLabels"]) {
        assert.ok(k in params.leafSize, k);
      }
    },
  },
];

test("打开、编辑、迁移、撤销重做之后 params 一个字段都不丢（两份样例）", () => {
  useManifestStore.getState().replaceBundle(manifest, 1);
  for (const sample of EDIT_SAMPLES) {
    const doc = sample.doc();
    const before = JSON.stringify(doc.params);
    const g = useGraphStore.getState();
    g.loadDoc(structuredClone(doc), "g.lyflow.json");
    assert.equal(JSON.stringify(saved().params), before, `${sample.name}：打开即保存`);

    sample.edit(g);
    assert.equal(JSON.stringify(saved().params), before, `${sample.name}：别的编辑不碰图参数`);

    for (let i = 0; i < sample.undos; i++) g.undo();
    assert.equal(JSON.stringify(saved().params), before, `${sample.name}：撤销之后`);
    g.redo();
    assert.equal(JSON.stringify(saved().params), before, `${sample.name}：重做之后`);
    sample.check?.(saved().params);
  }
});

test("没有顶层 params 的图不会凭空多出这个键", () => {
  const doc = structuredClone(example);
  delete doc.params;
  useGraphStore.getState().loadDoc(doc, null);
  useGraphStore.getState().setName("x");
  assert.equal("params" in saved(), false);
});
