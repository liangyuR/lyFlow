// 定时备份（lib/autosave.ts）：存过盘的写 `<file>~`、没存过盘的写到传输层给的那一处；找回来、换上、删掉。
// 传输层是内存里的假实现 —— 真的 app data 目录、真的写盘在 e2e m3 的 suitePanels 里走一遍。
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";

import {
  askRestoreUntitled,
  autosaveTick,
  discardUntitledBackup,
  findUntitledBackup,
  openSourceFor,
  restoreUntitled,
  untitledBackupPath,
} from "../src/lib/autosave.ts";
import { browserDialogs, setDialogs } from "../src/lib/dialogs.ts";
import { registerModalHost, settleModal, useModalStore } from "../src/lib/modal.ts";
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

test("备份生命周期：未保存/已保存写入条件、恢复后的未保存保护、撤销与清理", async () => {
  const t = fresh();
  await autosaveTick();
  assert.equal(t.files.size, 0, "空白新图不备份");
  g().addNode("filter.voxel_grid", { x: 0, y: 0 });
  await autosaveTick();
  assert.deepEqual([...t.files].map(([path, entry]) => [path, entry.doc.nodes.length]), [[`${UNTITLED}~`, 1]], "非空未保存图写入传输层给出的路径");

  g().newDoc();
  const backup = await findUntitledBackup();
  assert.deepEqual([backup?.path, backup?.loaded.doc.nodes.length, backup?.savedAt], [UNTITLED, 1, t.files.get(`${UNTITLED}~`).at], "重新打开后找到备份及时间");
  restoreUntitled(backup);
  assert.deepEqual([g().doc.nodes.length, g().filePath, g().dirty, g().savedDoc], [1, null, true, null], "恢复内容没有保存点，关闭前仍需确认");
  g().addNode("filter.voxel_grid", { x: 300, y: 0 });
  g().undo();
  assert.equal(g().dirty, true, "恢复后改动再撤销也不能变成已保存");
  await discardUntitledBackup();
  assert.deepEqual([t.files.size, await findUntitledBackup()], [0, null], "丢弃后不会再次发现备份");

  g().markSaved("g.lyflow.json");
  await autosaveTick();
  assert.deepEqual([g().dirty, t.files.size], [false, 0], "已保存且未改动不写备份");
  g().addNode("filter.voxel_grid", { x: 300, y: 0 });
  await autosaveTick();
  assert.deepEqual([...t.files.keys()], ["g.lyflow.json~"], "已保存文档的备份不写进未命名路径");
  g().markSaved("g.lyflow.json");
  g().markUnsaved();
  assert.deepEqual([g().dirty, g().filePath], [true, "g.lyflow.json"], "从文件备份恢复时保留文件路径并标为未保存");
});

test("打开时备份比正文新：编辑器里问恢复 / 丢弃 / 取消 —— 取消与 Esc 不打开、备份留着；丢弃才删；没挂对话框退回原生的是 / 否", async () => {
  const t = fresh();
  const P = "C:/w/点2.lyflow.json";
  const tick = () => new Promise((r) => setTimeout(r, 0));
  const unhost = registerModalHost();
  try {
    const cases = [
      // [说明, 点了哪个（null = Esc）, 期望从哪开（null = 不开）, 期望备份还在]
      ["恢复备份", "restore", "backup", true],
      ["丢弃备份，打开上次保存的", "discard", "file", false],
      ["取消", "cancel", null, true],
      ["Esc", null, null, true],
    ];
    for (const [name, choice, want, kept] of cases) {
      await t.writeBackup(P, g().doc);
      const pending = openSourceFor(P);
      await tick();
      const modal = useModalStore.getState().current;
      assert.deepEqual(modal?.choices.map((c) => c.id), ["restore", "discard", "cancel"], `${name}：恢复排第一（默认拿焦点）`);
      assert.match(modal.message, /^点2\.lyflow\.json 有一份比上次保存更新的自动备份（备份于 .+，文件上次保存于 不详）/, name);
      settleModal(choice);
      assert.deepEqual([await pending, t.files.has(`${P}~`)], [want, kept], name);
    }
    t.files.delete(`${P}~`);
    assert.equal(await openSourceFor(P), "file", "没有备份：不问，直接开正文");
    assert.equal(useModalStore.getState().current, null);

    // 开 app 时没存过盘的那张：同样三选一，取消就是先留着
    const untitled = askRestoreUntitled({ path: UNTITLED, loaded: { doc: g().doc, migrations: [] }, savedAt: null });
    assert.match(useModalStore.getState().current?.message ?? "", /要恢复吗？选「取消」备份先留着/);
    settleModal(null);
    assert.equal(await untitled, "cancel");
  } finally { unhost(); settleModal(null); }

  // 编辑器整个卸掉了（没挂对话框）：退回原生的是 / 否，否 = 丢弃（与以前一样）
  setDialogs({ ...browserDialogs, confirmRestore: async () => false });
  try {
    await t.writeBackup(P, g().doc);
    assert.deepEqual([await openSourceFor(P), t.files.has(`${P}~`)], ["file", false]);
  } finally { setDialogs(undefined); }
});

test("备份负向矩阵：不支持未命名备份、损坏内容与空图都不能恢复", async () => {
  for (const [name, options, empty] of [
    ["HTTP/静态快照没有未命名备份接口", { untitled: null }, false],
    ["备份读坏后清理，避免反复提示", { corrupt: true }, false],
    ["空图备份清理", {}, true],
  ]) {
    const t = fresh(options);
    if (empty) await t.writeBackup(UNTITLED, { schemaVersion: 1, id: "x", nodes: [], edges: [] });
    else {
      g().addNode("filter.voxel_grid", { x: 0, y: 0 });
      await autosaveTick();
    }
    if (options.untitled === null) assert.equal(await untitledBackupPath(), null, name);
    else assert.equal(t.files.size, 1, `${name}：已有待检查备份`);
    assert.deepEqual([await findUntitledBackup(), t.files.size], [null, 0], name);
  }
});

test("有没存的改动时问「保存 / 不保存 / 取消」：存成了或不保存才接着做；另存为取消了等于取消", async () => {
  fresh();
  try {
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
  } finally { settleModal(null); useGraphStore.setState({ dirty: false }); }
});
