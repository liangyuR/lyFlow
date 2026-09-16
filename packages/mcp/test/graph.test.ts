import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import test from "node:test";

import { applySet, checkStructure, envelopeGraphPath, resolveGraph } from "../src/graph.js";
import type { GraphDoc } from "../src/types.js";

function doc(): GraphDoc {
  return {
    schemaVersion: 1,
    id: "g",
    nodes: [
      { id: "gen", op: "gen.synthetic", params: { pointCount: 100, seed: 1 } },
      { id: "voxel", op: "filter.voxel_grid", params: { leafSize: [0.02, 0.02, 0.02] } },
    ],
    edges: [],
  };
}

test("set 按 <节点>.<参数> 改 doc，别的参数留着", () => {
  const d = applySet(doc(), { "gen.pointCount": 24000, "voxel.leafSize": [0.01, 0.01, 0.01] });
  assert.deepEqual(d.nodes[0]?.params, { pointCount: 24000, seed: 1 });
  assert.deepEqual(d.nodes[1]?.params, { leafSize: [0.01, 0.01, 0.01] });
});

test("set 的节点不存在就报错，键写错也报错", () => {
  assert.throws(() => applySet(doc(), { "nope.x": 1 }), /没有节点 nope/);
  assert.throws(() => applySet(doc(), { gen: 1 }), /<nodeId>\.<param>/);
  assert.throws(() => applySet(doc(), { "gen.": 1 }), /<nodeId>\.<param>/);
});

test("节点 id 里带点时按最后一个点切", () => {
  const d: GraphDoc = { nodes: [{ id: "a.b", op: "x", params: {} }] };
  applySet(d, { "a.b.leaf": 3 });
  assert.deepEqual(d.nodes[0]?.params, { leaf: 3 });
});

test("resolveGraph 不改调用方给的内联图", () => {
  const original = doc();
  const resolved = resolveGraph({ graph: original, set: { "gen.pointCount": 5 } });
  assert.equal(resolved.doc.nodes[0]?.params?.["pointCount"], 5);
  assert.equal(original.nodes[0]?.params?.["pointCount"], 100);
});

test("resolveGraph 从磁盘读 graphPath", () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "lyflow-mcp-test-"));
  const file = path.join(dir, "g.lyflow.json");
  fs.writeFileSync(file, JSON.stringify(doc()), "utf8");
  const resolved = resolveGraph({ graphPath: file });
  assert.equal(resolved.doc.nodes.length, 2);
  fs.rmSync(dir, { recursive: true, force: true });
});

test("graph 与 graphPath 只能给一个，都不给也报错", () => {
  assert.throws(() => resolveGraph({ graph: doc(), graphPath: "g.json" }), /只能给一个/);
  assert.throws(() => resolveGraph({}), /graph/);
});

test("GraphDoc 的必填字段缺了就当场报，节点缺 id/op 也报", () => {
  assert.throws(() => checkStructure({ nodes: [] }, "graph"), /schemaVersion、id、edges/);
  assert.throws(
    () =>
      checkStructure(
        { schemaVersion: 1, id: "g", nodes: [{ id: "a" } as unknown as GraphDoc["nodes"][0]], edges: [] },
        "graph",
      ),
    /第 0 个节点/,
  );
  checkStructure(doc(), "graph");
});

test("信封里的 graphPath 只用来定基准目录", () => {
  assert.equal(envelopeGraphPath(undefined, "configs/R1"), "configs/R1/graph.lyflow.json");
  assert.equal(envelopeGraphPath("sub/g.lyflow.json", undefined), "sub/g.lyflow.json");
  assert.equal(envelopeGraphPath("D:\\abs\\g.lyflow.json", undefined), null);
  assert.equal(envelopeGraphPath(undefined, undefined), null);
});
