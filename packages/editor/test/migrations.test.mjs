// ADR-0025：迁移诊断带 edits 时，applyMigrations 在同一条撤销记录里删边、插节点、加边，
// 语义与 bridge 的 GraphDoc::apply_migration（lyflow migrate --write）一致。
import assert from "node:assert/strict";
import { test } from "node:test";

import { useGraphStore } from "../src/store/graph.ts";

function doc() {
  return {
    schemaVersion: 1,
    id: "01J8XQZ4K7N3M2R5V8W1YB6TCF",
    nodes: [
      { id: "a", op: "t.src", ui: { position: { x: 0, y: 0 } } },
      { id: "b", op: "t.src", ui: { position: { x: 0, y: 200 } } },
      { id: "s", op: "t.sink", opVersion: "1.1.0", ui: { position: { x: 500, y: 100 } } },
    ],
    edges: [
      { id: "e1", from: { node: "a", port: "out" }, to: { node: "s", port: "old1" } },
      { id: "e2", from: { node: "b", port: "out" }, to: { node: "s", port: "old2" } },
      { id: "m_s_in", from: { node: "a", port: "out" }, to: { node: "b", port: "unrelated" } },
    ],
  };
}

const action = {
  kind: "migration",
  nodeId: "s",
  severity: "warning",
  phase: "validate",
  code: "migration",
  message: "节点已从 t.sink v1.1.0 迁移到 t.sink v2.0.0",
  op: "t.sink",
  opVersion: "2.0.0",
  params: {},
  notes: ["old1、old2 收进新插的 t.pack，接到 in"],
  edits: {
    removeEdges: [
      { id: "e1", from: { node: "a", port: "out" }, to: { node: "s", port: "old1" } },
      { id: "e2", from: { node: "b", port: "out" }, to: { node: "s", port: "old2" } },
    ],
    addNodes: [{ id: "s_pack", op: "t.pack", opVersion: "1.0.0", params: {}, title: "打包", near: "s" }],
    addEdges: [
      { id: "m_s_pack_x", from: { node: "a", port: "out" }, to: { node: "s_pack", port: "x" } },
      { id: "m_s_pack_y", from: { node: "b", port: "out" }, to: { node: "s_pack", port: "y" } },
      { id: "m_s_in", from: { node: "s_pack", port: "out" }, to: { node: "s", port: "in" } },
    ],
  },
};

test("edits：删两条旧边、插一个节点、加三条边，一次撤销全部回去", () => {
  const g = useGraphStore.getState();
  const before = doc();
  g.loadDoc(structuredClone(before), "g.lyflow.json");
  assert.equal(g.applyMigrations([action]), 1);

  const after = JSON.parse(JSON.stringify(useGraphStore.getState().doc));
  assert.equal(after.nodes.find((n) => n.id === "s").opVersion, "2.0.0");
  const pack = after.nodes.find((n) => n.id === "s_pack");
  assert.deepEqual(pack, {
    id: "s_pack",
    op: "t.pack",
    opVersion: "1.0.0",
    params: {},
    ui: { position: { x: 280, y: 240 }, title: "打包" },
  });
  const wires = after.edges.map((e) => `${e.id}:${e.from.node}.${e.from.port}>${e.to.node}.${e.to.port}`);
  // m_s_in 已被一条无关的边占了，新边加后缀
  assert.deepEqual(wires, [
    "m_s_in:a.out>b.unrelated",
    "m_s_pack_x:a.out>s_pack.x",
    "m_s_pack_y:b.out>s_pack.y",
    "m_s_in_2:s_pack.out>s.in",
  ]);
  assert.equal(useGraphStore.getState().dirty, true);

  useGraphStore.getState().undo();
  assert.deepEqual(JSON.parse(JSON.stringify(useGraphStore.getState().doc)), before);
});

test("不带 edits 的迁移照旧只改参数", () => {
  const g = useGraphStore.getState();
  g.loadDoc(doc(), "g.lyflow.json");
  const { edits: _, ...plain } = action;
  g.applyMigrations([plain]);
  const after = useGraphStore.getState().doc;
  assert.equal(after.nodes.length, 3);
  assert.equal(after.edges.length, 3);
});
