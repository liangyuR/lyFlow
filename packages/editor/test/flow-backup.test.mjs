import assert from "node:assert/strict";
import { test } from "node:test";

import { clearFlowBackup, readFlowBackup, writeFlowBackup } from "../src/lib/flowBackup.ts";

const emptyRecipes = () => ({ recipes: [], defaultName: null });

function workspace() {
  const tab = (id, filePath) => ({
    id,
    title: `工作流 ${id}`,
    doc: { schemaVersion: 1, id: `graph-${id}`, nodes: [{ id: "n1", op: "util.reroute" }], edges: [] },
    filePath,
    dirty: true,
    savedDoc: null,
    recipeDirty: false,
    recipe: { set: emptyRecipes(), saved: emptyRecipes(), current: null, currentId: null, dir: null, savedFiles: [], diskText: [] },
  });
  return { version: 1, activeId: "tab-b", tabs: [tab("tab-a", null), tab("tab-b", "C:/项目/检测.lyflow.json")] };
}

function memoryBackend() {
  return {
    value: null,
    async read() { return this.value === null ? null : JSON.parse(this.value); },
    async write(payload) { this.value = JSON.stringify(payload); },
    async clear() { this.value = null; },
  };
}

test("工作区备份同时保留多个未保存文档、活动tab及配方独立保存基线", async () => {
  const backend = memoryBackend();
  const payload = workspace();
  payload.tabs.push({ ...structuredClone(payload.tabs[0]), id: "tab-c", doc: { ...payload.tabs[0].doc, id: "graph-c" } });
  const tab = payload.tabs[1];
  tab.dirty = false;
  tab.savedDoc = structuredClone(tab.doc);
  tab.recipeDirty = true;
  tab.recipe.set.recipes.push({ id: "r1", name: "高速", values: { threshold: 12 }, graph: null, updatedAt: null });
  tab.recipe.current = "高速";
  tab.recipe.currentId = "r1";
  tab.recipe.dir = "C:/项目/检测.recipes";
  tab.recipe.savedFiles = [["r1", "原配方.lyflow-recipe.json"]];
  tab.recipe.diskText = [["原配方.lyflow-recipe.json", '{"values":{"threshold":8}}']];
  tab.recipe.baseEdits = { "n1.threshold": { nodeId: "n1", param: "threshold", path: [], before: 8, after: 12, recipe: "高速" } };

  assert.equal(await writeFlowBackup(payload, backend), true);
  const restored = await readFlowBackup(backend);
  assert.deepEqual(restored, payload);
  restored.tabs[1].recipe.set.recipes[0].values.threshold = 99;
  assert.equal((await readFlowBackup(backend)).tabs[1].recipe.set.recipes[0].values.threshold, 12);
  assert.equal(await clearFlowBackup(backend), true);
  assert.equal(await readFlowBackup(backend), null);
});

test("备份安全矩阵：损坏载荷不覆盖/恢复，配额与权限失败可重试且不冒充成功", async () => {
  const backend = memoryBackend();
  const valid = workspace();
  assert.equal(await writeFlowBackup(valid, backend), true);
  const original = backend.value;
  const invalid = [
    ["无载荷", null],
    ["不支持的版本", { ...valid, version: 2 }],
    ["活动tab缺失", { ...valid, activeId: "missing" }],
    ["空tab集合", { ...valid, tabs: [] }],
    ["重复tab", { ...valid, tabs: [valid.tabs[0], valid.tabs[0]] }],
    ["文档缺少schema", { ...valid, activeId: "tab-a", tabs: [{ ...valid.tabs[0], doc: { nodes: [], edges: [] } }] }],
    ["损坏节点", { ...valid, activeId: "tab-a", tabs: [{ ...valid.tabs[0], doc: { ...valid.tabs[0].doc, nodes: [null] } }] }],
    ["损坏配方", { ...valid, activeId: "tab-a", tabs: [{ ...valid.tabs[0], recipe: { ...valid.tabs[0].recipe, set: { recipes: [null], defaultName: null } } }] }],
    ["配方文件映射非数组", { ...valid, activeId: "tab-a", tabs: [{ ...valid.tabs[0], recipe: { ...valid.tabs[0].recipe, savedFiles: {} } }] }],
    ["配方保存状态缺失", { ...valid, activeId: "tab-a", tabs: [{ ...valid.tabs[0], recipeDirty: undefined }] }],
  ];
  for (const [name, payload] of invalid) {
    assert.deepEqual([await writeFlowBackup(payload, backend), backend.value], [false, original], `${name}：不覆盖已有备份`);
    backend.value = JSON.stringify(payload);
    assert.equal(await readFlowBackup(backend), null, `${name}：不恢复损坏载荷`);
    backend.value = original;
  }

  await clearFlowBackup(backend);
  let fail = true;
  const write = backend.write.bind(backend);
  backend.write = async (payload) => {
    if (fail) throw new Error("QuotaExceededError");
    await write(payload);
  };
  assert.equal(await writeFlowBackup(workspace(), backend), false);
  assert.equal(await readFlowBackup(backend), null);
  fail = false;
  assert.equal(await writeFlowBackup(workspace(), backend), true);
  assert.deepEqual(await readFlowBackup(backend), workspace());
  backend.read = async () => { throw new Error("SecurityError"); };
  backend.clear = async () => { throw new Error("SecurityError"); };
  assert.equal(await readFlowBackup(backend), null);
  assert.equal(await clearFlowBackup(backend), false);
});

test("关闭清理排在正在写入的备份后面，旧写入不会在清理后复活，入队时已冻结文档", async () => {
  let release;
  let gate = new Promise((resolve) => { release = resolve; });
  let value = null;
  const calls = [];
  const backend = {
    async write(payload) {
      calls.push("write");
      await gate;
      value = structuredClone(payload);
    },
    async clear() { calls.push("clear"); value = null; },
    async read() { calls.push("read"); return value; },
  };
  const payload = workspace();
  const writing = writeFlowBackup(payload, backend);
  payload.tabs[0].doc.nodes.push({ id: "late", op: "util.reroute" });
  await Promise.resolve();
  release();
  assert.equal(await writing, true);
  assert.equal((await readFlowBackup(backend)).tabs[0].doc.nodes.length, 1);

  gate = new Promise((resolve) => { release = resolve; });
  const writingAgain = writeFlowBackup(workspace(), backend);
  const clearing = clearFlowBackup(backend);
  const reading = readFlowBackup(backend);
  await Promise.resolve();
  assert.deepEqual(calls, ["write", "read", "write"]);
  release();
  assert.deepEqual(await Promise.all([writingAgain, clearing, reading]), [true, true, null]);
  assert.deepEqual(calls, ["write", "read", "write", "clear", "read"]);
});

test("IndexedDB不可用时没有内存或localStorage兜底冒充持久保存", { skip: typeof indexedDB !== "undefined" }, async () => {
  assert.equal(await writeFlowBackup(workspace()), false);
  assert.equal(await readFlowBackup(), null);
  assert.equal(await clearFlowBackup(), false);
});
