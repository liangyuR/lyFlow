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
  for (const [query, text, indices] of [
    ["ＶＧ", "Voxel Grid", [0, 6]],
    ["vg", "Ｖｏｘｅｌ　Ｇｒｉｄ", [0, 6]],
    ["ffi", "oﬃce", [1]],
    ["abc", "a b abc", [4, 5, 6]],
  ]) assert.deepEqual(fuzzyMatch(query, text)?.indices, indices, `${query}：归一化后的高亮仍指向原文，优先连续命中`);
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

  const bilingual = [
    op("register.icp_2d", "ICP 配准 2D", ["点云配准", "Iterative Closest Point"], "根据最近邻迭代对齐。"),
    op("image.gaussian_blur", "高斯平滑", ["Gaussian Blur", "高斯滤波"], "抑制图像中的噪声。"),
    op("io.load_pcd", "读取点云 PCD", ["Load PCD", "点云文件"], "从磁盘读取PCD文件。"),
  ];
  for (const [query, expected] of [
    ["closest 配准", ["register.icp_2d"]],
    ["配准 closest", ["register.icp_2d"]],
    ["２Ｄ　ＩＣＰ", ["register.icp_2d"]],
    ["ＰＣＤ　读取", ["io.load_pcd"]],
    ["blur 高斯", ["image.gaussian_blur"]],
    ["高斯 blu", ["image.gaussian_blur"]],
    ["clsoest 配准", ["register.icp_2d"]],
    ["gausian", ["image.gaussian_blur"]],
    ["gaussain", ["image.gaussian_blur"]],
    ["gausssian", ["image.gaussian_blur"]],
    ["iteratvie", ["register.icp_2d"]],
    ["closest 不存在", []],
    ["xyz 配准", []],
    ["icpx", []],
    ["gauszzan", []],
  ]) assert.deepEqual(searchOperators(bilingual, query).map((hit) => hit.op.id), expected, `${query}：多词跨字段 AND，中英文归一化与有限拼写容错`);

  assert.deepEqual(searchOperators(bilingual, "PCD 磁盘").map(({ fieldIndex, indices }) => ({ fieldIndex, indices })), [{ fieldIndex: 0, indices: [5, 6, 7] }], "混合名称与说明时只返回主字段的原文高亮下标");
  assert.deepEqual(searchOperators(bilingual, "读取 PCD").map(({ fieldIndex, indices }) => ({ fieldIndex, indices })), [{ fieldIndex: 0, indices: [0, 1, 5, 6, 7] }], "多个词在同一主字段命中时合并高亮");

  const ranked = [
    op("fixture.description", "说明命中", [], "voxel"),
    op("fixture.fuzzy", "VolXEL"),
    op("fixture.keyword", "体素降采样", ["voxel"]),
    op("fixture.name", "voxel"),
    op("fixture.long", "Voxel Grid"),
  ];
  assert.deepEqual(searchOperators(ranked, "voxel").slice(0, 3).map((hit) => hit.op.id), ["fixture.name", "fixture.keyword", "fixture.long"], "精确名称、关键词与完整单词优先于弱子序列和说明");
  const chineseNames = [
    op("transform.make", "刚体变换构造", ["平移"]),
    op("edit.translate_region", "点云选区平移"),
    op("dts.seal_dome", "胶条主体提取", ["圆拟合"]),
    op("fit.circle_2d", "二维圆拟合"),
  ];
  for (const [query, expected] of [
    ["平移", ["edit.translate_region", "transform.make"]],
    ["圆拟合", ["fit.circle_2d", "dts.seal_dome"]],
  ]) assert.deepEqual(searchOperators(chineseNames, query).map((hit) => hit.op.id), expected, `${query}：中文名称没有空格，连续命中优先于其它算子的完整关键词`);
  assert.deepEqual(searchOperators([op("fixture.negative", "Load Scale Blur")], "road"), [], "不把首字母不同的另一个英文词当成拼写错误");
  assert.deepEqual(searchOperators([op("fixture.negative", "Very broad explanatory text with scattered letters")], "vort"), [], "拒绝说明中跨度过大的偶然子序列");
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
  assert.deepEqual(found("gap 剖面"), ["gap.measure-skeleton"], "片段同样支持跨 id 与说明的多词搜索");
  assert.deepEqual(found("圆 ＧＡＰ"), ["gap.gap-circles"], "片段支持乱序和全半角归一化");
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
