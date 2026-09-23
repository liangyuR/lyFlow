// m7-plan J7/J8、§1 验收 7：顶层图参数 `params` 编辑器不解释，只原样往返。
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

test("样例图带顶层 params，fixture 里至少两个", () => {
  assert.ok(example.params && Object.keys(example.params).length > 0);
  assert.equal(Object.keys(fixture().params).length, 2);
});

test("打开即保存：params 原样保留", () => {
  const doc = fixture();
  const before = JSON.stringify(doc.params);
  useGraphStore.getState().loadDoc(structuredClone(doc), "g.lyflow.json");
  const out = saved();
  assert.deepEqual(out.params, doc.params);
  assert.equal(JSON.stringify(out.params), before);
  assert.deepEqual(out, doc);
});

test("编辑、迁移、撤销重做之后 params 仍原样保留", () => {
  useManifestStore.getState().replaceBundle(manifest, 1);
  const doc = fixture();
  const before = JSON.stringify(doc.params);
  const g = useGraphStore.getState();
  g.loadDoc(structuredClone(doc), "g.lyflow.json");

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
  assert.equal(JSON.stringify(edited.params), before);

  g.undo();
  g.undo();
  assert.equal(JSON.stringify(saved().params), before);
  g.redo();
  assert.equal(JSON.stringify(saved().params), before);
});

test("没有顶层 params 的图不会凭空多出这个键", () => {
  const doc = structuredClone(example);
  delete doc.params;
  useGraphStore.getState().loadDoc(doc, null);
  useGraphStore.getState().setName("x");
  assert.equal("params" in saved(), false);
});
