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
    // [说明, 输出类型, 期望, 输入类型（可选）]
    ["点云", ["PointCloud"], "cloud"],
    ["只有 2D 几何：叠在上游借来的底图上", ["Box2D", "Line2D"], "cloud"],
    ["Indices 指向一片云，底图有意义", ["Indices", "Plane"], "cloud"],
    ["点云 + 量测：点云优先", ["PointCloud", "Measurement"], "cloud"],
    ["只有 Record（字符串 / JSON）", ["Record"], "value"],
    ["量测与平面", ["Measurement", "Plane"], "value"],
    ["张量在主预览里只看得到值", ["Tensor"], "value"],
    ["输出图像：图像模式", ["Image"], "image"],
    ["图像域的像素几何（找圆 / 区域统计）：画在输入那张图上", ["Measurement", "Box2D"], "image", ["Image", "Image"]],
    ["输入是图像、输出是点云（深度图转点云）：点云优先", ["PointCloud"], "cloud", ["Image"]],
    ["没有图像的 2D 几何照旧叠在点云底图上", ["Box2D"], "cloud", ["PointCloud"]],
    ["Bundle 里有点云字段", ["Bundle<gap.ScanPair>"], "cloud"],
    ["Bundle 里有 2D 几何字段", ["Bundle<t.Rois>"], "cloud"],
    ["Bundle 里全是值", ["Bundle<t.Info>"], "value"],
    ["不认识的 Bundle 按老行为走点云场景", ["Bundle<t.Unknown>"], "cloud"],
    ["Any 还没跑过，类型未知：按老行为", ["Any", "Record"], "cloud"],
    ["没有输出端口", [], "cloud"],
  ];
  const wrong = cases
    .map(([name, types, want, inputs]) => [name, viewerContentFor(types, bundles, inputs), want])
    .filter(([, got, want]) => got !== want);
  assert.deepEqual(wrong, [], "说明 / 实际 / 期望");
});

test("对比的两栏：一侧可画就是点云场景，两侧都只有值才是值表格", () => {
  const got = [["cloud", "cloud"], ["cloud", "value"], ["value", "cloud"], ["value", "value"]].map(
    ([a, b]) => compareContentFor(a, b),
  );
  assert.deepEqual(got, ["cloud", "cloud", "cloud", "value"]);
});

// 换了一片云要不要重新取景（lib/viewFit）。修前每片新云都取景：重跑、在链上逐个点节点都把视角拉回斜 45°
test("sameFrame：包围盒相交、对角线差不到 4 倍才算同一个坐标系（不重新取景）", async () => {
  const { sameFrame } = await import("../src/lib/viewFit.ts");
  const box = (x0, y0, z0, x1, y1, z1) => [x0, y0, z0, x1, y1, z1];
  const base = box(0, 0, 0, 10, 10, 2);
  const cases = [
    ["同一片", base, true],
    ["抽稀之后小一圈", box(0.2, 0.1, 0, 9.8, 9.9, 1.9), true],
    ["只截了一部分（大小差 3 倍）", box(0, 0, 0, 3.5, 3.5, 1), true],
    ["平移 100 m", box(100, 0, 0, 110, 10, 2), false],
    ["放大 1000 倍", box(0, 0, 0, 10000, 10000, 2000), false],
    ["小到 1/10（差 10 倍）", box(0, 0, 0, 1, 1, 0.2), false],
    ["退化成一个点、与一片云", box(5, 5, 1, 5, 5, 1), false],
  ];
  for (const [name, other, want] of cases) assert.equal(sameFrame(base, other), want, name);
});
