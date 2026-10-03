// param-recipe P1.3 / P1.4 / P1.6：图参数的 store 动作。纯逻辑，不开浏览器 ——
// 界面上的右键「纳入配方」、被绑定行的显示与编辑、运行结果逐位相同在 scripts/e2e/params_p1.mjs 里走真实 app。
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";

import { copyParams, pasteParams, resetParams } from "../src/lib/editActions.ts";
import { effectiveGraphValues, resolveGraphBinding, specFromParam } from "../src/lib/graphParams.ts";
import { planParamPaste } from "../src/lib/paramClipboard.ts";
import { stepHistory } from "../src/lib/history.ts";
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

test("多选一起改（检查器）：同一个参数写进每个节点，一条撤销；数字框拖动的外层事务里就并进去", () => {
  reset();
  const both = () => [node("n_clean").params.leafSize, node("n_clean2").params.leafSize];
  const steps = g().past.length;
  g().setParamMany(["n_clean", "n_clean2"], "leafSize", [0.02, 0.02, 0.02]);
  assert.deepEqual(both(), [[0.02, 0.02, 0.02], [0.02, 0.02, 0.02]]);
  assert.equal(g().past.length, steps + 1);
  assert.equal(g().past.at(-1).label, "修改 2 个节点的 leafSize");
  g().undo();
  assert.deepEqual(both(), [[0.01, 0.01, 0.01], undefined], "一次 Ctrl+Z 两个一起还原");

  g().begin();
  for (const v of [0.03, 0.04]) g().setParamMany(["n_clean", "n_clean2"], "leafSize", [v, v, v]);
  g().commit("拖动参数");
  assert.equal(g().past.length, steps + 1);
  assert.equal(g().past.at(-1).label, "拖动参数");
  assert.deepEqual(both(), [[0.04, 0.04, 0.04], [0.04, 0.04, 0.04]]);

  // 撤销 / 重做带一句 toast 说是哪一步（Ctrl+Z 是盲按的）；栈空时什么都不做
  stepHistory("undo");
  assert.equal(useUiStore.getState().toast?.text, "已撤销：拖动参数");
  stepHistory("redo");
  assert.equal(useUiStore.getState().toast?.text, "已重做：拖动参数");
  useUiStore.getState().hideToast();
  stepHistory("redo");
  assert.equal(useUiStore.getState().toast, null, "没有可重做的：不弹");

  // 被图参数绑定的那个照样改图参数，不写成节点显式值（每个节点都走 setParam 的路由）
  g().promoteToGraphParam("n_voxel", "leafSize");
  g().setParamMany(["n_voxel"], "leafSize", [0.07, 0.07, 0.07]);
  assert.deepEqual(doc().params.leafSize.default, [0.07, 0.07, 0.07]);
  assert.equal(node("n_voxel").params.leafSize, undefined);
});

test("右键复制 / 粘贴参数：稀疏的展开、只粘同一种算子、一条撤销；被图参数或子图参数提供的不动", () => {
  reset();
  const ui = () => useUiStore.getState();
  assert.equal(copyParams("n_voxel"), true);
  assert.deepEqual(ui().paramClipboard.values, { leafSize: [0.005, 0.005, 0.005], minPointsPerVoxel: 0 }, "没写的参数按默认值带上");

  const n2 = g().addNode("filter.voxel_grid", { x: 0, y: 300 });
  g().setParam(n2, "minPointsPerVoxel", 3);
  const steps = g().past.length;
  pasteParams([n2, "n_plane"]);
  assert.deepEqual(node(n2).params, { leafSize: [0.005, 0.005, 0.005] }, "minPoints 回到默认就删键（稀疏）");
  assert.deepEqual(node("n_plane").params, { constraint: "axis" }, "不是同一种算子：没动");
  assert.deepEqual([g().past.length - steps, g().past.at(-1).label], [1, "粘贴参数"]);
  assert.match(ui().toast.text, /1 个节点不是同一种算子/);
  pasteParams([n2]);
  assert.equal(g().past.length - steps, 1, "已经一样：不记空撤销");
  g().undo();
  assert.deepEqual(node(n2).params, { minPointsPerVoxel: 3 }, "一次撤销还原");

  // 被图参数绑定的：复制取图参数此刻的值；粘贴时不动它（图参数是共享的，不该被一次粘贴悄悄改掉）
  g().promoteToGraphParam("n_voxel", "leafSize");
  g().setGraphParamDefault("leafSize", [0.03, 0.03, 0.03]);
  copyParams("n_voxel");
  assert.deepEqual(ui().paramClipboard.values.leafSize, [0.03, 0.03, 0.03]);
  copyParams(n2);
  const plan = planParamPaste(doc(), [], ui().paramClipboard, ["n_voxel"], useManifestStore.getState().operatorsById);
  assert.deepEqual([plan.locked, plan.edits.map((e) => e.name)], [1, ["minPointsPerVoxel"]]);

  // 子图里：提升成子图参数的那个不复制、也不粘
  reset();
  copyParams("n_voxel");
  inClean();
  pasteParams(["s_voxel"]);
  assert.deepEqual(node("s_voxel", doc().subgraphs.sg_clean).params, {}, "leafSize 由子图参数提供，minPoints 本来就一样");
  copyParams("s_voxel");
  assert.equal("leafSize" in ui().paramClipboard.values, false);

  // 子图节点的 id 只在一张图里有意义：换了一张图不粘
  reset();
  copyParams("n_clean");
  const other = fixture();
  other.id = "another-doc";
  useGraphStore.getState().loadDoc(other, "h.lyflow.json");
  const ops = useManifestStore.getState().operatorsById;
  assert.equal(planParamPaste(doc(), [], ui().paramClipboard, ["n_clean2"], ops).edits.length, 0);
});

test("右键全部恢复默认：节点上写着的都删掉，一条撤销；被图参数提供的不动", () => {
  reset();
  const before = structuredClone(doc());
  const steps = g().past.length;
  resetParams(["n_voxel", "n_plane", "n_clean"]);
  assert.deepEqual([node("n_voxel").params, node("n_plane").params, node("n_clean").params], [{}, {}, {}]);
  assert.equal(doc().params.planeTol.default, 0.006, "n_plane.distanceThreshold 绑着的图参数不动");
  assert.deepEqual([g().past.length - steps, g().past.at(-1).label], [1, "3 个节点恢复默认参数"]);
  resetParams(["n_voxel"]);
  assert.equal(g().past.length - steps, 1, "已经是默认：不记空撤销");
  g().undo();
  assert.deepEqual(doc(), before, "一次撤销还原");
});

test("换成别的算子：id、标题照旧；同名同类型的线留下、别的断开；绑定、图输出跟着收拾；子图出口会断就不换；一条撤销", () => {
  reset();
  const out = g().markGraphOutput({ node: "n_plane", port: "rest" });
  const before = structuredClone(doc());
  const steps = g().past.length;
  const plan = g().replaceNodeOp("n_plane", "filter.voxel_grid");
  const n = node("n_plane");
  assert.deepEqual([n.id, n.op, n.opVersion, n.params, n.ui.title], ["n_plane", "filter.voxel_grid", "1.2.0", {}, before.nodes[2].ui.title]);
  const wires = doc().edges.filter((e) => e.from.node === "n_plane" || e.to.node === "n_plane").map((e) => `${e.from.node}.${e.from.port}>${e.to.node}.${e.to.port}`);
  assert.deepEqual(wires, ["n_voxel.cloud>n_plane.cloud"], "进来的 cloud 留着；rest 新算子没有，断开");
  assert.deepEqual([plan.droppedParams, doc().params.planeTol.binds], [["constraint"], []], "参数与图参数的绑定跟着收拾，图参数本身留着");
  assert.deepEqual([out in (doc().outputs ?? {}), "cloud" in doc().outputs], [false, true], "指着 rest 的图输出删掉，别的不动");
  assert.deepEqual([g().past.length - steps, g().past.at(-1).label], [1, "换成 Voxel Grid"]);
  g().undo();
  assert.deepEqual(doc(), before, "一次撤销还原");

  // 子图里：提升的参数摘掉绑定（子图参数留着）、子图入口照旧接上、出去的线断开
  reset();
  inClean();
  g().replaceNodeOp("s_voxel", "segment.plane");
  const def = doc().subgraphs.sg_clean;
  assert.deepEqual([def.params[0].binds, def.inputs[0].to, def.edges.length], [[], [{ node: "s_voxel", port: "cloud" }], 0]);

  // 子图出口从这个节点出去、新算子没有那个输出：整个不换，说清楚为什么
  reset();
  inClean();
  const untouched = structuredClone(doc());
  const pastBefore = g().past.length;
  assert.equal(g().replaceNodeOp("s_sor", "segment.plane"), null);
  assert.match(g().lastRejection, /子图输出 cloud/);
  assert.deepEqual([doc(), g().past.length], [untouched, pastBefore]);
});

test("Ctrl+D 复制被图参数绑定的节点：副本写着此刻的有效值（不带绑定），不再回到算子默认值", () => {
  reset();
  g().promoteToGraphParam("n_voxel", "leafSize");
  g().setGraphParamDefault("leafSize", [0.03, 0.03, 0.03]);
  const copy = g().duplicateNodes(["n_voxel"]).nodeIds[0];
  assert.deepEqual(node(copy).params.leafSize, [0.03, 0.03, 0.03]);
  assert.deepEqual(doc().params.leafSize.binds, ["n_voxel.leafSize"], "副本不进 binds：绑定是图参数的属性");
  assert.equal(node("n_voxel").params.leafSize, undefined, "原件照旧由图参数提供");
});

test("Shift+D 复制并保留输入：副本接原件的同一个上游、输出空着；子图里接上子图入口；一条撤销", () => {
  const cases = [
    // [说明, 进子图, 复制谁, keepInputs, 期望]
    ["顶层一个", false, ["n_voxel"], true, { into: ["n_load.cloud>copy0.cloud"], out: 0, boundary: null, steps: 1, label: "复制并保留输入" }],
    ["顶层两个：选区里面的照旧连副本之间，外面进来的接上", false, ["n_voxel", "n_plane"], true,
      { into: ["copy0.cloud>copy1.cloud", "n_load.cloud>copy0.cloud"], out: 0, boundary: null, steps: 1, label: "复制 2 个节点并保留输入" }],
    ["不保留（Ctrl+D）照旧", false, ["n_voxel"], false, { into: [], out: 0, boundary: null, steps: 1, label: "粘贴节点" }],
    ["子图实例", false, ["n_clean"], true, { into: ["n_plane.rest>copy0.cloud"], out: 0, boundary: null, steps: 1, label: "复制并保留输入" }],
    ["子图里：接的是子图入口（inputs[].to）", true, ["s_voxel"], true,
      { into: [], out: 0, boundary: ["s_voxel.cloud", "copy0.cloud"], steps: 1, label: "复制并保留输入" }],
  ];
  for (const [name, inside, ids, keepInputs, want] of cases) {
    reset();
    if (inside) inClean();
    const lvl = () => (inside ? doc().subgraphs.sg_clean : doc());
    const before = structuredClone(doc());
    const steps = g().past.length;
    const r = g().duplicateNodes(ids, { keepInputs });
    const alias = new Map(r.nodeIds.map((id, i) => [id, `copy${i}`]));
    const nameOf = (id) => alias.get(id) ?? id;
    const got = {
      into: lvl().edges.filter((e) => alias.has(e.to.node)).map((e) => `${nameOf(e.from.node)}.${e.from.port}>${nameOf(e.to.node)}.${e.to.port}`).sort(),
      out: lvl().edges.filter((e) => alias.has(e.from.node) && !alias.has(e.to.node)).length,
      boundary: inside ? lvl().inputs[0].to.map((t) => `${nameOf(t.node)}.${t.port}`) : null,
      steps: g().past.length - steps,
      label: g().past.at(-1)?.label,
    };
    assert.deepEqual(got, want, name);
    if (inside) {
      // 删掉副本：子图入口里指着它的那条一起摘掉（以前留着，下次运行 core 报 unknown_port）
      g().deleteNodes(r.nodeIds);
      assert.deepEqual(lvl().inputs[0].to.map((t) => t.node), ["s_voxel"], `${name}：删掉副本`);
      g().undo();
    }
    g().undo();
    assert.deepEqual(doc(), before, `${name}：一次撤销完全还原`);
  }
});

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

test("绑定：已由图参数提供的不能再纳入、类型对不上的被拒；bindToGraphParam 成功时显式值删掉、值从此取图参数", () => {
  reset();
  assert.equal(g().promoteToGraphParam("n_plane", "distanceThreshold"), null);
  assert.match(g().lastRejection, /planeTol/);
  assert.equal(g().bindToGraphParam("planeTol", "n_plane", "maxIterations"), false, "float 图参数绑 int 参数");
  assert.match(g().lastRejection, /类型不同/);

  const name = g().promoteToGraphParam("n_voxel", "leafSize");
  assert.deepEqual(node("n_clean").params.leafSize, [0.01, 0.01, 0.01], "前提：n_clean 上写着显式值");
  assert.equal(g().bindToGraphParam(name, "n_clean", "leafSize"), true);
  assert.deepEqual(doc().params.leafSize.binds, ["n_voxel.leafSize", "n_clean.leafSize"]);
  assert.equal(g().past.at(-1).label, "绑定到图参数 leafSize");
  assert.equal(node("n_clean").params.leafSize, undefined, "显式值删掉：不会有 param_conflict");
  g().setParam("n_clean", "leafSize", [0.02, 0.02, 0.02]);
  assert.deepEqual(doc().params.leafSize.default, [0.02, 0.02, 0.02], "值从此取图参数");
  assert.equal(node("n_clean").params.leafSize, undefined);
});

test("解除绑定、删节点与删除图参数：当前值写回节点（稀疏存储照旧），行为不变", () => {
  reset();
  g().unbindFromGraphParam("planeTol", "n_plane.distanceThreshold");
  assert.equal(node("n_plane").params.distanceThreshold, 0.006, "当前值写回节点");
  assert.deepEqual(doc().params.planeTol.binds, []);
  g().undo();

  // 删节点：指着它的 bind 一并摘掉，图参数留着
  g().deleteNodes(["n_plane"]);
  assert.deepEqual(doc().params.planeTol.binds, []);
  g().undo();
  assert.deepEqual(doc().params.planeTol.binds, ["n_plane.distanceThreshold"]);

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

test("库算子展开为内联子图：定义换新 id 写进 doc，op 换成 sub:，参数与连线不动；不是库算子不改；一次撤销还原", () => {
  // 拿样例图里 sg_clean 的定义当「core 给的库定义」，把第二个实例改成库算子
  const lib = fixture();
  const n2 = lib.nodes.find((n) => n.id === "n_clean2");
  n2.op = "lib.clean";
  n2.params = { leafSize: [0.02, 0.02, 0.02] };
  reset(lib);
  const def = { ...structuredClone(example.subgraphs.sg_clean), category: "Cleanup", version: "1.2.0" };
  const before = structuredClone(doc());

  assert.equal(g().inlineLibrary("n_clean", def), null, "sub: 节点不是库算子");
  assert.equal(g().inlineLibrary("n_nope", def), null);
  assert.deepEqual(doc(), before, "没展开就什么都不改");

  const sgId = g().inlineLibrary("n_clean2", def);
  assert.ok(sgId && sgId !== "sg_clean", `新子图 id 不撞已有的：${sgId}`);
  const inlined = node("n_clean2");
  assert.deepEqual(
    [inlined.op, inlined.opVersion, inlined.params],
    [`sub:${sgId}`, undefined, { leafSize: [0.02, 0.02, 0.02] }],
    "op 换成 sub:、库的版本号不留、参数原样",
  );
  assert.deepEqual(doc().subgraphs[sgId], def, "定义原样写进去（version / category 跟着定义走）");
  assert.deepEqual(doc().edges, before.edges, "连线不动");
  assert.equal(g().past.at(-1)?.label, "展开库算子", "是一条撤销记录");

  g().undo();
  assert.deepEqual(doc(), before, "一次撤销回到库算子");
});
