// lib/fuzzy + lib/search：节点面板与画布搜索弹层共用的算子搜索（交互清单 P0 #9）。
// 钉住的是排序的几条规矩：名字命中比 id / 关键词 / 说明命中值钱，缩写与中文关键词都认，短的排前面。
import assert from "node:assert/strict";
import { test } from "node:test";

import { fuzzyMatch } from "../src/lib/fuzzy.ts";
import { FIELD_LABELS, searchOperators } from "../src/lib/search.ts";

const op = (id, label, keywords = [], doc = "") => ({ id, label, keywords, doc, category: "t", inputs: [], outputs: [], params: [] });
const ops = [
  op("filter.voxel_grid", "Voxel Grid", ["降采样", "体素"], "体素栅格降采样"),
  op("filter.voxel_grid_cov", "Voxel Grid Covariance", ["协方差"]),
  op("filter.statistical_outlier", "Statistical Outlier Removal", ["离群点", "去噪"], "按邻域距离的统计剔除离群点"),
  op("io.load_pcd", "Load PCD", ["读取", "pcd"]),
  op("segment.ransac_plane", "RANSAC Plane", ["平面"], "用 RANSAC 拟合平面，可以用来去掉地面"),
];
const ids = (q) => searchOperators(ops, q).map((h) => h.op.id);

test("fuzzyMatch：按顺序找到每个字符，大小写不敏感；缩写命中词首加分", () => {
  assert.equal(fuzzyMatch("xyz", "Voxel Grid"), null);
  assert.deepEqual(fuzzyMatch("vg", "Voxel Grid")?.indices, [0, 6], "v 是词首、g 是下一个词的词首");
  assert.ok(fuzzyMatch("vox", "Voxel Grid").score > fuzzyMatch("vxl", "Voxel Grid").score, "连着的比跳着的值钱");
  assert.deepEqual(fuzzyMatch("", "anything"), { score: 0, indices: [] });
});

test("searchOperators：名字命中排前、短的排前、中文关键词与说明也认，说明命中的标出是哪个字段", () => {
  assert.deepEqual(ids("voxel"), ["filter.voxel_grid", "filter.voxel_grid_cov"], "短的那个优先");
  assert.deepEqual(ids("vg")[0], "filter.voxel_grid", "缩写");
  assert.deepEqual(ids("离群"), ["filter.statistical_outlier"], "中文关键词");
  const ground = searchOperators(ops, "地面");
  assert.deepEqual(ground.map((h) => h.op.id), ["segment.ransac_plane"]);
  assert.equal(FIELD_LABELS[ground[0].fieldIndex], "说明", "只在说明里命中：行上标出来");
  assert.equal(searchOperators(ops, "pcd")[0].op.id, "io.load_pcd");
  assert.deepEqual(ids("   "), [], "空查询不搜（调用方自己列全部）");
});
