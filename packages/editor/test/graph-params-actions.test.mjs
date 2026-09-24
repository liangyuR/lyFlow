// param-recipe P1.3 / P1.4 / P1.6：图参数的 store 动作。纯逻辑，不开浏览器 ——
// 界面上的右键「纳入配方」、被绑定行的显示与编辑、运行结果逐位相同在 scripts/e2e/params_p1.mjs 里走真实 app。
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";

import { effectiveGraphValues, resolveGraphBinding, specFromParam } from "../src/lib/graphParams.ts";
import { useGraphStore } from "../src/store/graph.ts";
import { useManifestStore } from "../src/store/manifest.ts";
import { runParamsOf } from "../src/store/recipe.ts";
import { useUiStore } from "../src/store/ui.ts";

const root = new URL("../../../", import.meta.url);
const readJson = (rel) => JSON.parse(readFileSync(new URL(rel, root), "utf8"));

const manifest = readJson("schema/examples/manifest.example.json");
const example = readJson("schema/examples/graph.example.lyflow.json");
const voxelDecl = manifest.operators.find((o) => o.id === "filter.voxel_grid").params;

/** 样例图 + 同一个子图的第二个实例 n_clean2（验「其它实例行为不变」）。 */
function fixture() {
  const doc = structuredClone(example);
  doc.nodes.push({ id: "n_clean2", op: "sub:sg_clean", opVersion: "1.0.0", params: {}, ui: { position: { x: 1100, y: 0 } } });
  return doc;
}

function reset(doc = fixture()) {
  useManifestStore.getState().replaceBundle(structuredClone(manifest), 1);
  useUiStore.getState().setPath([]);
  useGraphStore.getState().loadDoc(doc, "g.lyflow.json");
}

const g = () => useGraphStore.getState();
const doc = () => g().doc;
const node = (id, lvl = doc()) => lvl.nodes.find((n) => n.id === id);
const inClean = () => useUiStore.getState().setPath([{ nodeId: "n_clean", subgraphId: "sg_clean" }]);

test("纳入配方：当前有效值成为 default、规格从 manifest 抄、节点显式值被删；一次撤销完全还原", () => {
  reset();
  const before = structuredClone(doc());
  const name = g().promoteToGraphParam("n_voxel", "leafSize");
  assert.equal(name, "leafSize");
  const gp = doc().params.leafSize;
  assert.deepEqual(gp.default, [0.005, 0.005, 0.005], "default = 纳入前节点上的显式值");
  assert.deepEqual(gp.binds, ["n_voxel.leafSize"]);
  assert.equal(gp.type, "vec3f");
  for (const k of ["min", "max", "step", "unit", "doc"]) assert.deepEqual(gp[k], voxelDecl[0][k], k);
  assert.match(gp.label, / · Leaf Size$/, "label = 节点标题 · 参数 label");
  assert.equal("name" in gp, false, "名字是键，不写进声明");
  assert.equal(node("n_voxel").params.leafSize, undefined, "显式值删掉：不会有 param_conflict");
  assert.equal(g().past.at(-1).label, "纳入配方 leafSize");

  g().undo();
  assert.deepEqual(doc(), before, "一次 Ctrl+Z 还原到纳入之前");
});

test("被绑定的参数上 setParam 改的是图参数的 default，不写成节点显式值", () => {
  reset();
  g().promoteToGraphParam("n_voxel", "leafSize");
  g().setParam("n_voxel", "leafSize", [0.02, 0.02, 0.02]);
  assert.deepEqual(doc().params.leafSize.default, [0.02, 0.02, 0.02]);
  assert.equal(node("n_voxel").params.leafSize, undefined);
  assert.equal(g().past.at(-1).label, "修改图参数 leafSize");
  // 拖动：begin/commit 之间每帧都改，整段一条撤销
  const steps = g().past.length;
  g().begin();
  for (const v of [0.03, 0.04, 0.05]) g().setParam("n_voxel", "leafSize", [v, v, v]);
  g().commit("拖动参数");
  assert.equal(g().past.length, steps + 1);
  assert.deepEqual(doc().params.leafSize.default, [0.05, 0.05, 0.05]);
});

test("子图内部参数纳入配方：内参 → 子图参数 → 这个实例上的图参数，一次撤销还原两级", () => {
  reset();
  const before = structuredClone(doc());
  inClean();
  const name = g().promoteToGraphParam("s_voxel", "minPointsPerVoxel");
  assert.equal(name, "minPointsPerVoxel");
  const sp = doc().subgraphs.sg_clean.params.find((p) => p.name === "minPointsPerVoxel");
  assert.ok(sp, "子图定义里多了一个提升参数");
  assert.deepEqual(sp.binds, [{ node: "s_voxel", param: "minPointsPerVoxel" }]);
  assert.equal(sp.default, 0, "子图参数的默认值 = 内参当前值：其它实例行为不变");
  const gp = doc().params.minPointsPerVoxel;
  assert.deepEqual(gp.binds, ["n_clean.minPointsPerVoxel"], "绑在「这个」实例上");
  assert.equal(gp.default, 0);
  assert.match(gp.label, /^去噪子图 \/ .* · Min Points \/ Voxel$/, "label 带上路径上的实例标题");
  assert.equal(node("n_clean2").params.minPointsPerVoxel, undefined, "另一个实例没被碰");
  // 内部这一行现在由图参数提供
  const binding = resolveGraphBinding(doc(), useUiStore.getState().path, "s_voxel", "minPointsPerVoxel");
  assert.deepEqual(binding, {
    graphParam: "minPointsPerVoxel",
    via: ["minPointsPerVoxel"],
    top: { node: "n_clean", param: "minPointsPerVoxel" },
  });
  assert.equal(g().past.at(-1).label, "纳入配方 minPointsPerVoxel");

  g().undo();
  assert.deepEqual(doc(), before, "子图参数与图参数两级一起撤掉");
});

test("子图里已经提升过的内参：只补最外一级，默认值取这个实例上的值", () => {
  reset();
  inClean();
  // sg_clean 的 leafSize 早就提升了，n_clean 上显式写着 [0.01, …]
  const name = g().promoteToGraphParam("s_voxel", "leafSize");
  assert.equal(name, "leafSize");
  assert.equal(doc().subgraphs.sg_clean.params.filter((p) => p.name.startsWith("leafSize")).length, 1, "没有再提升一次");
  assert.deepEqual(doc().params.leafSize.default, [0.01, 0.01, 0.01]);
  assert.equal(node("n_clean").params.leafSize, undefined, "实例上的显式值删掉");
  // 在内部那一行上编辑 → 改的是图参数
  g().setParam("s_voxel", "leafSize", [0.03, 0.03, 0.03]);
  assert.deepEqual(doc().params.leafSize.default, [0.03, 0.03, 0.03]);
  assert.equal(node("s_voxel", doc().subgraphs.sg_clean).params.leafSize, undefined, "内参上也没写");
  // 取消子图提升：顶层那条 bind 跟着走，不留 unknown_bind
  g().unpromoteParam("leafSize");
  assert.deepEqual(doc().params.leafSize.binds, []);
});

test("已经由图参数提供的参数不能再纳入一次；类型对不上的绑定被拒", () => {
  reset();
  assert.equal(g().promoteToGraphParam("n_plane", "distanceThreshold"), null);
  assert.match(g().lastRejection, /planeTol/);
  assert.equal(g().bindToGraphParam("planeTol", "n_plane", "maxIterations"), false, "float 图参数绑 int 参数");
  assert.match(g().lastRejection, /类型不同/);
});

test("bindToGraphParam：显式值删掉、值从此取图参数", () => {
  reset();
  const name = g().promoteToGraphParam("n_voxel", "leafSize");
  assert.equal(g().bindToGraphParam(name, "n_clean2", "leafSize"), true);
  assert.deepEqual(doc().params.leafSize.binds, ["n_voxel.leafSize", "n_clean2.leafSize"]);
  assert.equal(g().past.at(-1).label, "绑定到图参数 leafSize");
});

test("解除绑定与删除：当前值写回节点（稀疏存储照旧），行为不变", () => {
  reset();
  g().unbindFromGraphParam("planeTol", "n_plane.distanceThreshold");
  assert.equal(node("n_plane").params.distanceThreshold, 0.006, "当前值写回节点");
  assert.deepEqual(doc().params.planeTol.binds, []);
  g().undo();

  const name = g().promoteToGraphParam("n_voxel", "leafSize");
  g().setParam("n_voxel", "leafSize", [0.01, 0.01, 0.01]); // 等于算子默认值
  g().removeGraphParam(name);
  assert.equal(doc().params.leafSize, undefined);
  assert.equal(node("n_voxel").params.leafSize, undefined, "值回到算子默认：写回时删键");
  g().removeGraphParam("planeTol");
  assert.equal(node("n_plane").params.distanceThreshold, 0.006);
  assert.equal("params" in doc(), false, "最后一个删了，params 键也不留");
});

test("改名与改规格：名字规则、键的位置、undefined 删字段", () => {
  reset();
  g().promoteToGraphParam("n_voxel", "leafSize");
  for (const bad of ["", "a.b", "a=b", " x", "leafSize"]) {
    assert.equal(g().renameGraphParam("planeTol", bad), false, bad);
  }
  assert.equal(g().renameGraphParam("planeTol", "floorTol"), true);
  assert.deepEqual(Object.keys(doc().params), ["floorTol", "leafSize"], "改名不挪位置");

  g().setGraphParamSpec("leafSize", { label: "体素", group: "降采样", max: undefined });
  const gp = doc().params.leafSize;
  assert.equal(gp.label, "体素");
  assert.equal(gp.group, "降采样");
  assert.equal("max" in gp, false);
  g().setGraphParamSpec("leafSize", { binds: [], default: 1 });
  assert.deepEqual(gp.binds, doc().params.leafSize.binds, "规格补丁改不了 binds / default");
});

test("删节点：指着它的 bind 一并摘掉，图参数留着", () => {
  reset();
  g().deleteNodes(["n_plane"]);
  assert.deepEqual(doc().params.planeTol.binds, []);
});

test("运行传参：default ← 当前配方覆盖（P1 覆盖恒为空）；没有图参数时不传", () => {
  reset();
  assert.deepEqual(runParamsOf(doc()), { planeTol: 0.006 });
  assert.deepEqual(effectiveGraphValues(doc(), { planeTol: 0.01, gone: 1 }), { planeTol: 0.01 },
    "覆盖里多出来的名字不传给 core");
  const plain = structuredClone(example);
  delete plain.params;
  assert.equal(runParamsOf(plain), undefined);
});

test("specFromParam：同算子的联动条件与 roiBackdrop 不抄，semantic 照抄", () => {
  const spec = specFromParam(
    {
      name: "roi", type: "vec4f", label: "框", default: [0, 0, 1, 1], unit: "mm", semantic: "roi",
      visibleWhen: { param: "mode", eq: "a" }, enabledWhen: { param: "x", ne: 1 },
      roiBackdrop: { dir: "d" },
    },
    "节点 · 框",
  );
  assert.deepEqual(spec, { type: "vec4f", label: "节点 · 框", unit: "mm", semantic: "roi" });
});

test("撤销回到保存点时 dirty 复原（P1.6）", () => {
  reset();
  assert.equal(g().dirty, false);
  g().setName("改个名");
  assert.equal(g().dirty, true);
  g().undo();
  assert.equal(g().dirty, false, "回到打开时那一份");
  g().redo();
  assert.equal(g().dirty, true);
  g().markSaved("g.lyflow.json", doc());
  assert.equal(g().dirty, false);
  g().setName("再改");
  g().undo();
  assert.equal(g().dirty, false, "回到存盘时那一份");
  g().undo();
  assert.equal(g().dirty, true, "再往前就又和磁盘不一样了");
});

test("合成子图 / 解散子图：被绑定的参数跟着搬，bind 不悬空、值还归图参数管", () => {
  reset();
  g().promoteToGraphParam("n_voxel", "leafSize");
  const composed = g().composeSubgraph(["n_voxel", "n_plane"]);
  assert.ok(composed);
  const def = doc().subgraphs[composed.subgraphId];
  const names = def.params.map((p) => p.name).sort();
  assert.deepEqual(names, ["distanceThreshold", "leafSize"], "两个被绑定的内参都提升成外参");
  assert.deepEqual(doc().params.leafSize.binds, [`${composed.nodeId}.leafSize`]);
  assert.deepEqual(doc().params.planeTol.binds, [`${composed.nodeId}.distanceThreshold`]);
  assert.equal(def.params.find((p) => p.name === "distanceThreshold").default, 0.006);

  const inlined = g().dissolveSubgraph(composed.nodeId);
  assert.equal(inlined.length, 2);
  const [a, b] = [doc().params.leafSize.binds, doc().params.planeTol.binds];
  assert.equal(a.length, 1);
  assert.equal(b.length, 1);
  const voxel = node(a[0].split(".")[0]);
  const plane = node(b[0].split(".")[0]);
  assert.equal(voxel.op, "filter.voxel_grid");
  assert.equal(plane.op, "segment.plane");
  assert.equal(voxel.params.leafSize, undefined, "解散时不往被绑定的内参上写显式值");
  assert.equal(plane.params.distanceThreshold, undefined);
});
