// lib/fuzzy + lib/search：节点面板与画布搜索弹层共用的算子搜索（交互清单 P0 #9）。
// 钉住的是排序的几条规矩：名字命中比 id / 关键词 / 说明命中值钱，缩写与中文关键词都认，短的排前面。
import assert from "node:assert/strict";
import { test } from "node:test";

import { fuzzyMatch } from "../src/lib/fuzzy.ts";
import { FIELD_LABELS, searchableOps, searchOperators, searchSnippets, SNIPPET_FIELD_LABELS } from "../src/lib/search.ts";

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

test("searchSnippets：名称、id、分类、说明都认（与算子同一套模糊匹配），名称命中的排前、标出命中的字段", () => {
  const snip = (id, label, category = "", doc = "") => ({ id, label, category, doc, nodes: [] });
  const snippets = [
    snip("gap.measure-skeleton", "测点骨架", "gap", "读点云 → 定位 → 剖面"),
    snip("gap.gap-circles", "间隙 · 圆", "gap", "两个圆的间隙"),
    snip("user.mine", "我的骨架", "用户"),
  ];
  const found = (q) => searchSnippets(snippets, q).map((h) => h.snippet.id);
  assert.deepEqual(found("骨架"), ["gap.measure-skeleton", "user.mine"], "名称命中，短的在前");
  assert.deepEqual(found("circles"), ["gap.gap-circles"], "id");
  assert.deepEqual(found("用户"), ["user.mine"], "分类");
  assert.equal(SNIPPET_FIELD_LABELS[searchSnippets(snippets, "剖面")[0].fieldIndex], "说明");
  assert.deepEqual(found("  "), [], "空查询不搜（弹层自己列全部）");
});

test("searchableOps：manifest 的算子加上这张图的子图；会套进当前这几层自己的不列", () => {
  const base = [op("filter.voxel_grid", "Voxel Grid")];
  const def = (name, nodes) => ({ name, nodes, edges: [], inputs: [], outputs: [], params: [] });
  const subgraphs = {
    A: def("外层", [{ id: "a1", op: "sub:B", params: {} }]),
    B: def("内层", [{ id: "b1", op: "filter.voxel_grid", params: {} }]),
    C: def("别的", []),
  };
  const list = (around) => searchableOps(base, subgraphs, new Set(around)).map((o) => o.id);
  assert.deepEqual(list([]), ["filter.voxel_grid", "sub:A", "sub:B", "sub:C"], "顶层：子图都能加");
  assert.deepEqual(list(["A"]), ["filter.voxel_grid", "sub:B", "sub:C"], "在 A 里：A 不列");
  assert.deepEqual(list(["A", "B"]), ["filter.voxel_grid", "sub:C"], "在 A 里的 B 里：A（里面有 B）、B 都不列");
  assert.equal(searchableOps(base, subgraphs, new Set()).find((o) => o.id === "sub:A")?.label, "外层", "名字是子图的名字");
  assert.deepEqual(searchableOps(base, undefined, new Set()).map((o) => o.id), ["filter.voxel_grid"]);
});
