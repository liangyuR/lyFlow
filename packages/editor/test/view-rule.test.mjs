// 主预览按节点的输出类型选内容（lib/viewRule）：点云场景还是值的表格。
// 纯逻辑；连线查看器按端口选视图的那半（defaultViewFor）由 e2e peek.mjs / m8b.mjs 走真界面。
import assert from "node:assert/strict";
import { test } from "node:test";

import { compareContentFor, viewerContentFor } from "../src/lib/viewRule.ts";

const bundles = [
  { kind: "gap.ScanPair", fields: [{ name: "primary", type: "PointCloud" }] },
  { kind: "t.Info", fields: [{ name: "info", type: "Record" }, { name: "m", type: "Measurement" }] },
  { kind: "t.Rois", fields: [{ name: "datum", type: "Box2D" }, { name: "info", type: "Record" }] },
];

test("主预览：有一个可画的端口就显示点云场景，全是值才换成表格", () => {
  const cases = [
    // [说明, 输出类型, 期望]
    ["点云", ["PointCloud"], "cloud"],
    ["只有 2D 几何：叠在上游借来的底图上", ["Box2D", "Line2D"], "cloud"],
    ["Indices 指向一片云，底图有意义", ["Indices", "Plane"], "cloud"],
    ["点云 + 量测：点云优先", ["PointCloud", "Measurement"], "cloud"],
    ["只有 Record（字符串 / JSON）", ["Record"], "value"],
    ["量测与平面", ["Measurement", "Plane"], "value"],
    ["张量在主预览里只看得到值", ["Tensor"], "value"],
    ["图像在主预览里暂时只看得到尺寸与统计量（图像模式是 image-plan 阶段 3）", ["Image"], "value"],
    ["Bundle 里有点云字段", ["Bundle<gap.ScanPair>"], "cloud"],
    ["Bundle 里有 2D 几何字段", ["Bundle<t.Rois>"], "cloud"],
    ["Bundle 里全是值", ["Bundle<t.Info>"], "value"],
    ["不认识的 Bundle 按老行为走点云场景", ["Bundle<t.Unknown>"], "cloud"],
    ["Any 还没跑过，类型未知：按老行为", ["Any", "Record"], "cloud"],
    ["没有输出端口", [], "cloud"],
  ];
  const wrong = cases
    .map(([name, types, want]) => [name, viewerContentFor(types, bundles), want])
    .filter(([, got, want]) => got !== want);
  assert.deepEqual(wrong, [], "说明 / 实际 / 期望");
});

test("对比的两栏：一侧可画就是点云场景，两侧都只有值才是值表格", () => {
  const got = [["cloud", "cloud"], ["cloud", "value"], ["value", "cloud"], ["value", "value"]].map(
    ([a, b]) => compareContentFor(a, b),
  );
  assert.deepEqual(got, ["cloud", "cloud", "cloud", "value"]);
});
