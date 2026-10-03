// 定时备份（lib/autosave.ts）：存过盘的写 `<file>~`、没存过盘的写到传输层给的那一处；找回来、换上、删掉。
// 传输层是内存里的假实现 —— 真的 app data 目录、真的写盘在 e2e m3 的 suitePanels 里走一遍。
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";

import {
  autosaveTick,
  discardUntitledBackup,
  findUntitledBackup,
  restoreUntitled,
  untitledBackupPath,
} from "../src/lib/autosave.ts";
import { settleModal, useModalStore } from "../src/lib/modal.ts";
import { resolveUnsaved } from "../src/lib/unsaved.ts";
import { useGraphStore } from "../src/store/graph.ts";
import { useManifestStore } from "../src/store/manifest.ts";
import { setTransport } from "../src/transport/index.ts";

const root = new URL("../../../", import.meta.url);
const manifest = JSON.parse(readFileSync(new URL("schema/examples/manifest.example.json", root), "utf8"));
const UNTITLED = "C:/appdata/untitled.lyflow.json";

/** 只实现备份那几个口；`untitled: null` 模拟没有那个口的传输（HTTP、静态快照）。 */
function fakeTransport({ untitled = UNTITLED, corrupt = false } = {}) {
  const files = new Map(); // `<path>~` → { doc, at }
  let clock = 1_000;
  const t = {
    files,
    async writeBackup(path, doc) {
      files.set(`${path}~`, { doc: structuredClone(doc), at: clock++ });
    },
    async backupStatus(path) {
      const f = files.get(`${path}~`);
      return { exists: Boolean(f), newer: Boolean(f), backupModified: f?.at ?? null, fileModified: null };
    },
    async readBackup(path) {
      const f = files.get(`${path}~`);
      if (!f || corrupt) throw new Error("读不出来");
      return { doc: structuredClone(f.doc), migrations: [] };
    },
    async discardBackup(path) {
      files.delete(`${path}~`);
    },
  };
  if (untitled) t.untitledBackupPath = async () => untitled;
  return t;
}

const g = () => useGraphStore.getState();

function fresh(transportOpts) {
  const t = fakeTransport(transportOpts);
  setTransport(t);
  useManifestStore.getState().replaceBundle(structuredClone(manifest), 1);
  g().newDoc();
  return t;
}

test("没存过盘：有改动、不是空图才写，写在传输层给的那一处旁边；空白的新图不写", async () => {
  const t = fresh();
  await autosaveTick();
  assert.equal(t.files.size, 0, "空白的新图不备份");
  g().addNode("filter.voxel_grid", { x: 0, y: 0 });
  assert.equal(g().dirty, true);
  await autosaveTick();
  assert.deepEqual([...t.files.keys()], [`${UNTITLED}~`]);
  assert.equal(t.files.get(`${UNTITLED}~`).doc.nodes.length, 1);
});

test("存过盘的图写 `<file>~`，不碰没存过盘的那一处；没改动不写", async () => {
  const t = fresh();
  g().addNode("filter.voxel_grid", { x: 0, y: 0 });
  g().markSaved("g.lyflow.json");
  await autosaveTick();
  assert.equal(t.files.size, 0, "刚存过盘：没改动");
  g().addNode("filter.voxel_grid", { x: 300, y: 0 });
  await autosaveTick();
  assert.deepEqual([...t.files.keys()], ["g.lyflow.json~"]);
});

test("找回来、换上：没有路径、算没保存（撤销回不到「已保存」）；删掉之后再找是 null", async () => {
  const t = fresh();
  g().addNode("filter.voxel_grid", { x: 0, y: 0 });
  await autosaveTick();
  g().newDoc(); // 相当于重开 app：一张空白的新图
  const backup = await findUntitledBackup();
  assert.ok(backup);
  assert.equal(backup.path, UNTITLED);
  assert.equal(backup.loaded.doc.nodes.length, 1);
  assert.equal(backup.savedAt, t.files.get(`${UNTITLED}~`).at);

  restoreUntitled(backup);
  assert.equal(g().doc.nodes.length, 1);
  assert.equal(g().filePath, null);
  assert.equal(g().dirty, true, "内容不在盘上：标题要带 *、关窗口要问");
  assert.equal(g().savedDoc, null);
  // 换上之后接着改、再撤销回来：仍然没保存
  g().addNode("filter.voxel_grid", { x: 300, y: 0 });
  g().undo();
  assert.equal(g().dirty, true);

  await discardUntitledBackup();
  assert.equal(t.files.size, 0);
  assert.equal(await findUntitledBackup(), null);
});

test("传输层没有 untitledBackupPath（HTTP、静态快照）：没存过盘的图不备份，也找不到", async () => {
  const t = fresh({ untitled: null });
  assert.equal(await untitledBackupPath(), null);
  g().addNode("filter.voxel_grid", { x: 0, y: 0 });
  await autosaveTick();
  assert.equal(t.files.size, 0);
  assert.equal(await findUntitledBackup(), null);
});

test("备份读不出来或是空图：当作没有，顺手删掉", async () => {
  const t = fresh({ corrupt: true });
  g().addNode("filter.voxel_grid", { x: 0, y: 0 });
  await autosaveTick();
  assert.equal(t.files.size, 1);
  assert.equal(await findUntitledBackup(), null);
  assert.equal(t.files.size, 0, "读坏了的删掉，免得每次开 app 都问");

  const t2 = fresh();
  await t2.writeBackup(UNTITLED, { schemaVersion: 1, id: "x", nodes: [], edges: [] });
  assert.equal(await findUntitledBackup(), null);
  assert.equal(t2.files.size, 0);
});

test("markUnsaved：从 `<file>~` 恢复出来的图保留路径、算没保存", () => {
  fresh();
  g().addNode("filter.voxel_grid", { x: 0, y: 0 });
  g().markSaved("g.lyflow.json");
  assert.equal(g().dirty, false);
  g().markUnsaved();
  assert.equal(g().dirty, true);
  assert.equal(g().filePath, "g.lyflow.json");
});

test("有没存的改动时问「保存 / 不保存 / 取消」：存成了或不保存才接着做；另存为取消了等于取消", async () => {
  const cases = [
    // [说明, 点了哪个（null = Esc）, 保存的结果, 期望能接着做, 期望调了保存]
    ["保存、存成了", "save", true, true, true],
    ["保存、另存为对话框取消了（或写盘出错）", "save", false, false, true],
    ["不保存", "discard", true, true, false],
    ["取消", "cancel", true, false, false],
    ["Esc", null, true, false, false],
  ];
  for (const [name, choice, saved, want, wantSave] of cases) {
    useGraphStore.setState({ dirty: true });
    let calls = 0;
    const pending = resolveUnsaved("新建", async () => {
      calls += 1;
      return saved;
    });
    const modal = useModalStore.getState().current;
    assert.deepEqual(modal?.choices.map((c) => c.id), ["save", "discard", "cancel"], `${name}：保存排第一（默认拿焦点）`);
    assert.match(modal.message, /这张图有改动还没保存。新建之前/, name);
    settleModal(choice);
    assert.deepEqual([await pending, calls === 1], [want, wantSave], name);
  }
  useGraphStore.setState({ dirty: false });
  assert.equal(await resolveUnsaved("新建", async () => assert.fail("没有改动不该存")), true, "没有改动：不问、直接接着做");
  assert.equal(useModalStore.getState().current, null);
});
