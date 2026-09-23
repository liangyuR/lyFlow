// m8-plan L20 / L21：2D 拖框按底图分组（一次只处理一个模板槽）、「复制到其它槽」、标签互不遮挡。
// 纯逻辑；画布上的真实切换与拖动在 scripts/e2e/m8c.mjs 里走。
import assert from "node:assert/strict";
import { test } from "node:test";

import { copyFrameWrites, frameKeyOfGroup, pickFrame, placeLabels, roiFramesOf } from "../src/lib/roiFrames.ts";

// locate_template 的参数形状，缩到两个槽、每槽两个框
const roi = (name, slot, extra = {}) => ({
  name,
  type: "vec4f",
  unit: "mm",
  semantic: "roi",
  group: `模板槽 ${slot}`,
  default: [0, 0, 0, 0],
  roiBackdrop: { dir: "templateDir", files: [`t${slot}Left`, `t${slot}Right`], label: `模板 ${slot}`, labelParam: `t${slot}Id` },
  ...extra,
});
const text = (name, def) => ({ name, type: "string", default: def });
const shown = (slot) => ({ visibleWhen: { param: `t${slot}Enabled`, eq: true } });
const op = {
  id: "t.locate",
  version: "2.0.0",
  label: "locate",
  category: "t",
  inputs: [],
  outputs: [],
  params: [
    { name: "templateDir", type: "path", default: "" },
    text("t1Id", "f1"), text("t1Left", "l1.pcd"), text("t1Right", "r1.pcd"),
    roi("t1Datum", 1), roi("t1Target", 1),
    { name: "t2Enabled", type: "bool", default: false, group: "模板槽 2" },
    text("t2Id", "f2"), text("t2Left", "l2.pcd"), text("t2Right", "r2.pcd"),
    roi("t2Datum", 2, shown(2)), roi("t2Target", 2, shown(2)),
    { name: "overall", type: "vec4f", unit: "mm", semantic: "roi", default: [-1, -1, 1, 1] },
  ],
};
const node = (params) => ({ id: "n", op: "t.locate", params });

test("每个启用的槽一组框，标签是「模板 k · Id」，底图是那个槽的左右模板；数据坐标系的框不混进来", () => {
  const one = roiFramesOf(op, node({ templateDir: "D:/tpl" }));
  assert.equal(one.length, 1, "槽 2 没启用：只有槽 1 一组");
  assert.equal(one[0].label, "模板 1 · f1");
  assert.deepEqual(one[0].params.map((p) => p.name), ["t1Datum", "t1Target"]);
  assert.deepEqual(one[0].files, ["D:/tpl/l1.pcd", "D:/tpl/r1.pcd"]);

  const two = roiFramesOf(op, node({ templateDir: "D:/tpl", t2Enabled: true, t2Id: "big" }));
  assert.deepEqual(two.map((f) => f.label), ["模板 1 · f1", "模板 2 · big"]);
  assert.deepEqual(two[1].params.map((p) => p.name), ["t2Datum", "t2Target"]);
  assert.ok(two.every((f) => f.params.every((p) => p.name !== "overall")));
});

test("选过的组还在就用它，槽被关掉了就退回第一组", () => {
  const two = roiFramesOf(op, node({ t2Enabled: true }));
  assert.equal(pickFrame(two, two[1].key).label, "模板 2 · f2");
  const one = roiFramesOf(op, node({}));
  assert.equal(pickFrame(one, two[1].key).label, "模板 1 · f1");
  assert.equal(pickFrame([], "x"), null);
});

test("Inspector 的参数组认得出属于哪一组框（没启用的槽也认得出）", () => {
  const slot2 = op.params.filter((p) => p.group === "模板槽 2");
  const key = frameKeyOfGroup(slot2);
  const two = roiFramesOf(op, node({ t2Enabled: true }));
  assert.equal(key, two[1].key);
  assert.equal(frameKeyOfGroup([op.params[0]]), null);
});

test("复制到其它槽：按组内顺序一一对应、值取当前组的有效值，只写启用的槽", () => {
  const n = node({ t2Enabled: true, t1Datum: [-15, 163, -5, 167] });
  const frames = roiFramesOf(op, n);
  const writes = copyFrameWrites(op, n, frames[0], frames);
  assert.deepEqual(writes, [
    { param: "t2Datum", value: [-15, 163, -5, 167] },
    { param: "t2Target", value: [0, 0, 0, 0] },
  ]);
  assert.deepEqual(copyFrameWrites(op, node({}), roiFramesOf(op, node({}))[0], roiFramesOf(op, node({}))), []);
});

const intersects = (a, b) => a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
const labelRect = (box, spot) => ({
  x0: box.left + spot.dx, y0: box.top + spot.dy,
  x1: box.left + spot.dx + box.labelWidth, y1: box.top + spot.dy + box.labelHeight,
});

test("标签互不遮挡（L21）：两个挨着的框、标签比框宽时，第二个挪开", () => {
  // 截图里的情形：Seam Right 窄、紧挨着右边的 Target，标签比框宽，默认都放在框上方就会压住
  const boxes = [
    { left: 100, top: 50, width: 30, height: 40, labelWidth: 80, labelHeight: 14 },
    { left: 132, top: 60, width: 120, height: 40, labelWidth: 60, labelHeight: 14 },
  ];
  const spots = placeLabels(boxes);
  assert.equal(spots[0].where, "above");
  assert.notEqual(spots[1].where, "above");
  assert.ok(!intersects(labelRect(boxes[0], spots[0]), labelRect(boxes[1], spots[1])));
});

test("标签互不遮挡：四个框挤在一起时两两都不相交；离得远的都在框上方", () => {
  const crowded = [0, 1, 2, 3].map((i) => ({
    left: 100 + i * 20, top: 80, width: 18, height: 12, labelWidth: 70, labelHeight: 14,
  }));
  const spots = placeLabels(crowded);
  for (let i = 0; i < 4; i += 1) {
    for (let j = i + 1; j < 4; j += 1) {
      assert.ok(!intersects(labelRect(crowded[i], spots[i]), labelRect(crowded[j], spots[j])), `${i}-${j}`);
    }
  }
  const apart = [0, 1, 2].map((i) => ({
    left: i * 200, top: 80, width: 50, height: 30, labelWidth: 60, labelHeight: 14,
  }));
  assert.ok(placeLabels(apart).every((s) => s.where === "above"));
});
