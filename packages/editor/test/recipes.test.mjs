// param-recipe P3：配方的纯逻辑与 store 动作。文件格式、规格摘要（specDigest）的规范化、四类失配对着共享夹具
// （schema/fixtures/recipes/，P4 的 CLI 用同一份）、撤销（K7）、编辑语义（K4 / K6）。
// 界面上的下拉框、矩阵、管理页、Ctrl+S 落盘与外部修改检测在 scripts/e2e/params_p3.mjs 里走真实 app。
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { readdirSync, readFileSync } from "node:fs";
import { test } from "node:test";

import {
  applyFixes,
  EMPTY_SET,
  graphRefOf,
  parseIndex,
  parseRecipeText,
  recipeDirOf,
  recipeNameProblem,
  recipeReport,
  serializeIndex,
  serializeRecipe,
  specCanonical,
  specDigest,
  withValue,
} from "../src/lib/recipes.ts";
import { sha256Hex } from "../src/lib/sha256.ts";
import { useGraphStore } from "../src/store/graph.ts";
import { useManifestStore } from "../src/store/manifest.ts";
import {
  currentRecipeBlocker,
  recipesDirty,
  resetRecipes,
  runParamsOf,
  selectRecipe,
  useRecipeStore,
} from "../src/store/recipe.ts";
import { useUiStore } from "../src/store/ui.ts";

const root = new URL("../../../", import.meta.url);
const readText = (rel) => readFileSync(new URL(rel, root), "utf8");
const readJson = (rel) => JSON.parse(readText(rel));

const FIX = "schema/fixtures/recipes/";
const fixtureDoc = readJson(`${FIX}graph.lyflow.json`);
const expected = readJson(`${FIX}expected.json`);

// ------------------------------------------------------------ SHA-256

function verifyCases(cases, arrange) {
  for (const [name, verify, fresh = true] of cases) {
    if (fresh) arrange?.();
    try { verify(); }
    catch (error) { error.message = `${name}：${error.message}`; throw error; }
  }
}

test("sha256：标准测试向量、跨块的长输入、中文（UTF-8 字节）与 node:crypto 逐字节相同", () => {
  assert.equal(sha256Hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  assert.equal(sha256Hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  for (const s of ["x".repeat(55), "x".repeat(56), "x".repeat(64), "车型A·左前门".repeat(40), "𝒳 emoji 😀"]) {
    assert.equal(sha256Hex(s), createHash("sha256").update(s, "utf8").digest("hex"), JSON.stringify(s.slice(0, 20)));
  }
});

// ------------------------------------------------------------ specDigest 的规范化

const gp = (extra = {}) => ({ default: 1, binds: ["n.a"], ...extra });

test("specDigest跨语言契约：共享摘要、规格敏感性、Unicode排序与数值规范化", () => {
  verifyCases([
    ["specDigest：与共享夹具里记着的规范形和摘要一致（P4 的 Rust 实现对着同一个数）", () => {
      assert.equal(specCanonical(fixtureDoc), expected.specCanonical);
      assert.equal(specDigest(fixtureDoc), expected.specDigest);
      assert.match(specDigest(fixtureDoc), /^sha256:[0-9a-f]{64}$/);
    }, false],
    ["specDigest：只看名字、类型、min、max、options 的 value；书写顺序与展示字段不算规格", () => {
      const one = (name, spec) => ({ params: { [name]: gp(spec) } });
      const float01 = { type: "float", min: 0, max: 1 };
      const enumAB = { type: "enum", options: [{ value: "a", label: "A" }, { value: "b", label: "B" }] };
      // [说明, a, b, 摘要应当相同？]
      const cases = [
        ["图参数的书写顺序", { params: { x: gp(float01), y: gp({ type: "int" }) } }, { params: { y: gp({ type: "int" }), x: gp(float01) } }, true],
        [
          "label / 单位 / group / 软限位 / step / default / binds / doc / advanced",
          { params: { x: gp(float01), y: gp({ type: "int" }) } },
          {
            params: {
              x: gp({ ...float01, label: "别的名字", unit: "mm", group: "g", softMin: 0.2, softMax: 0.8, step: 0.1, default: 0.5, binds: ["m.b", "k.c"], doc: "…" }),
              y: gp({ type: "int", advanced: true }),
            },
          },
          true,
        ],
        ["options 的顺序与 label", one("m", enumAB), one("m", { type: "enum", options: [{ value: "b", label: "乙" }, { value: "a", label: "甲" }] }), true],
        ["options 少了一项", one("m", enumAB), one("m", { type: "enum", options: [{ value: "a", label: "A" }] }), false],
        ["名字", one("m", enumAB), one("n", enumAB), false],
        ["加了 min", one("x", { type: "float", min: 0 }), one("x", { type: "float" }), false],
        ["max 变了", one("x", { type: "float", max: 1 }), one("x", { type: "float", max: 2 }), false],
        ["类型", one("x", { type: "float" }), one("x", { type: "int" }), false],
        // 没有 type 的老格式：只有名字算数（min / max 在 core 里本来就不算规格）
        ["老格式的 max", one("x", { max: 5 }), one("x", {}), true],
        ["老格式补上 type", one("x", {}), one("x", { type: "float" }), false],
      ];
      for (const [what, a, b, same] of cases) {
        if (same) assert.equal(specDigest(a), specDigest(b), `${what}：不该改摘要`);
        else assert.notEqual(specDigest(a), specDigest(b), `${what}：该改摘要`);
      }
      // 没有图参数也有一个确定的摘要（空数组）
      assert.equal(specCanonical({}), "[]");
    }, false],
    ["specDigest：名字按码点序排（BMP 外的字符也与 Rust 的字节序一致），数用 ECMAScript 的格式", () => {
      // U+FF5E（BMP 内，UTF-16 码元 0xFF5E）与 U+1F600（BMP 外，码元 0xD83D…）：码元序里后者在前，码点序里在后
      const doc = { params: { "\u{1F600}": gp({ type: "float" }), "\uFF5E": gp({ type: "float", min: 0.1, max: 1e21 }) } };
      const rows = JSON.parse(specCanonical(doc));
      assert.deepEqual(rows.map((r) => r[0]), ["\uFF5E", "\u{1F600}"]);
      assert.ok(specCanonical(doc).includes("0.1,1e+21"), specCanonical(doc));
    }, false]
  ], setup);
});

// ------------------------------------------------------------ 四类失配：共享夹具

test("失配诊断与修复：共享四类报告、原型键安全、按建议修复及未涉及值不变", () => {
  const files = readdirSync(new URL(`${FIX}graph.recipes/`, root)).filter((f) => f.endsWith(".lyflow-recipe.json"));
  assert.deepEqual(files.sort(), Object.keys(expected.recipes).sort(), "夹具目录与 expected.json 列的文件对不上");
  for (const file of files) {
    const parsed = parseRecipeText(readText(`${FIX}graph.recipes/${file}`), file.replace(".lyflow-recipe.json", ""));
    assert.ok(!("error" in parsed), file);
    const entry = { id: "t", name: file, values: parsed.values, graph: parsed.graph, updatedAt: null };
    const before = recipeReport(fixtureDoc, entry);
    const want = expected.recipes[file];
    assert.equal(before.blocking, want.blocking, `${file} 的 blocking`);
    assert.deepEqual(
      before.items.map((m) => ({ kind: m.kind, param: m.param, fix: m.fix, message: m.message, fixLabel: m.fixLabel })),
      want.items,
      `${file} 的条目与修复文案`,
    );
    for (const m of before.items) assert.ok(m.message && m.fixLabel, `${file} ${m.param} 缺文案`);
    const fixed = applyFixes(entry, before.items, fixtureDoc, "2026-09-24T00:00:00.000Z");
    const after = recipeReport(fixtureDoc, fixed);
    assert.equal(after.items.length, 0, `${file} 修复后：${JSON.stringify(after.items)}`);
    if (before.items.length === 0) assert.equal(fixed, entry, `${file}：没失配就不该改`);
    else assert.deepEqual(fixed.graph, graphRefOf(fixtureDoc), file);
    const touched = new Set(before.items.map((m) => m.param).filter(Boolean));
    for (const [key, value] of Object.entries(entry.values)) {
      if (!touched.has(key)) assert.deepEqual(fixed.values[key], value, `${file}：${key} 不在报告里却被改了`);
    }
  }
  const values = JSON.parse('{"constructor": 1, "toString": 2, "__proto__": 3}');
  const inherited = recipeReport(fixtureDoc, { values, graph: graphRefOf(fixtureDoc) });
  assert.deepEqual(inherited.items.map((m) => [m.kind, m.param]),
    [["extra", "__proto__"], ["extra", "constructor"], ["extra", "toString"]], "只认图参数自己的键");
  assert.equal(inherited.blocking, 3, "原型键也是多余参数");
});

// ------------------------------------------------------------ 文件格式、目录、名字

test("配方文件契约：稀疏值和字段顺序、schema、配方/index往返及非法输入", () => {
  let entry = { id: "a", name: "车型A·左前门", values: {}, graph: graphRefOf(fixtureDoc), updatedAt: "2026-09-24T01:02:03.000Z" };
  entry = withValue(entry, fixtureDoc, "cutMax", 2.5);
  entry = withValue(entry, fixtureDoc, "leafSize", [0.03, 0.03, 0.03]);
  entry = withValue(entry, fixtureDoc, "tint", [0.29, 0.62, 1]);
  assert.deepEqual(Object.keys(entry.values), ["cutMax", "leafSize"]);
  assert.equal(withValue(entry, fixtureDoc, "cutMax", 1.2).values.cutMax, undefined, "写回基础值 = 删掉覆盖");
  const text = serializeRecipe(entry, fixtureDoc);
  assert.ok(text.endsWith("\n"));
  const raw = JSON.parse(text);
  assert.deepEqual(Object.keys(raw), ["schemaVersion", "name", "graph", "values", "updatedAt"]);
  assert.deepEqual(Object.keys(raw.values), ["leafSize", "cutMax"], "按 doc.params 的顺序");
  assert.equal(raw.graph.specDigest, expected.specDigest);
  const back = parseRecipeText(text, "别的名字");
  assert.equal(back.name, entry.name);
  assert.deepEqual(back.values, entry.values);
  assert.deepEqual(back.notes, []);

  const schema = readJson("schema/recipe.schema.json");
  for (const [name, value] of [["不带备注", raw], ["带备注", JSON.parse(serializeRecipe({ ...entry, note: "备注" }, fixtureDoc))]]) {
    for (const key of schema.required) assert.ok(key in value, `${name}缺必填字段 ${key}`);
    for (const key of Object.keys(value)) assert.ok(key in schema.properties, `${name}多余字段 ${key}`);
    assert.equal(value.schemaVersion, schema.properties.schemaVersion.const, name);
    assert.match(value.name, new RegExp(schema.properties.name.pattern), name);
    assert.match(value.graph.specDigest, new RegExp(schema.properties.graph.properties.specDigest.pattern), name);
    assert.deepEqual(Object.keys(value.graph).sort(), [...schema.properties.graph.required].sort(), name);
    for (const key of Object.keys(value.values)) assert.match(key, new RegExp(schema.properties.values.propertyNames.pattern), name);
    assert.ok(!Number.isNaN(Date.parse(value.updatedAt)), `${name}时间戳`);
  }
  const namePattern = new RegExp(schema.properties.name.pattern);
  for (const bad of ["a/b", "a\\b", "a:b", "a*b", "a?b", 'a"b', "a<b", "a>b", "a|b", "a\u0001b"]) {
    assert.ok(!namePattern.test(bad), `schema 该拒 ${JSON.stringify(bad)}`);
    assert.ok(recipeNameProblem(EMPTY_SET, bad), `编辑器该拒 ${JSON.stringify(bad)}`);
  }
  const set = { recipes: [entry, { ...entry, id: "b", name: "车型B" }], defaultName: "车型B" };
  const index = JSON.parse(serializeIndex(set));
  assert.deepEqual(index, { default: "车型B", order: [entry.name, "车型B"] });
  assert.deepEqual(parseIndex(serializeIndex(set)), { defaultName: "车型B", order: [entry.name, "车型B"] });
  const indexSchema = readJson("schema/recipe-index.schema.json");
  for (const key of Object.keys(index)) assert.ok(key in indexSchema.properties, `index.json 多了 ${key}`);
  assert.deepEqual(parseIndex("坏的"), { defaultName: null, order: [] });
  for (const [name, input] of [["无效JSON", "{"], ["数组", "[1]"], ["不支持版本", '{"schemaVersion": 9}']]) {
    assert.ok("error" in parseRecipeText(input, "x"), name);
  }
  const loose = parseRecipeText('{"values": {"a": 1}, "extra": 1}', "文件名");
  assert.equal(loose.name, "文件名");
  assert.equal(loose.graph, null);
  assert.equal(loose.notes.length, 3);
});

test("配方文件命名：目录推导、Windows文件名与大小写冲突", () => {
  verifyCases([
    ["配方目录：图文件同目录的 <图文件名去扩展名>.recipes/，分隔符跟着图路径走", () => {
      assert.equal(recipeDirOf("C:\\工程\\车门缝隙.lyflow.json"), "C:\\工程\\车门缝隙.recipes");
      assert.equal(recipeDirOf("graphs/demo.LYFLOW.json"), "graphs/demo.recipes");
      assert.equal(recipeDirOf("demo.json"), "demo.recipes");
      assert.equal(recipeDirOf("/srv/a.b/g"), "/srv/a.b/g.recipes");
    }, false],
    ["配方名：按 Windows 文件名的规矩、重名不分大小写、不能叫「基础」", () => {
      const set = { recipes: [{ id: "1", name: "车型A", values: {}, graph: null, updatedAt: null }], defaultName: null };
      assert.equal(recipeNameProblem(set, "车型B·右前门"), null);
      assert.ok(recipeNameProblem(set, "车型a"));
      assert.equal(recipeNameProblem(set, "车型A", "车型A"), null, "改名时跟自己不算重名");
      for (const bad of ["", " a", "a/b", "a:b", "a?", "CON", "lpt1", "a.", "基础"]) {
        assert.ok(recipeNameProblem(EMPTY_SET, bad), JSON.stringify(bad));
      }
    }, false]
  ], setup);
});

// ------------------------------------------------------------ store：编辑语义与撤销（K4、K6、K7）

const manifest = readJson("schema/examples/manifest.example.json");
const example = readJson("schema/examples/graph.example.lyflow.json");
const g = () => useGraphStore.getState();
const r = () => useRecipeStore.getState();

/** 样例图 + 两个图参数（voxel 的 leafSize、planeTol）+ 一个配方目录（已经「存过盘」）。 */
function setup() {
  useManifestStore.getState().replaceBundle(structuredClone(manifest), 1);
  useUiStore.getState().setPath([]);
  g().loadDoc(structuredClone(example), "g.lyflow.json");
  resetRecipes("g.recipes", "ready");
  const name = g().promoteToGraphParam("n_voxel", "leafSize");
  assert.equal(name, "leafSize");
  // 从这里开始算：纳入配方那一步不算在下面的撤销里
  useGraphStore.setState({ past: [], future: [] });
}

test("配方生命周期：创建/切换/覆盖/管理动作和撤销重做", () => {
  verifyCases([
    ["新建配方、写值、撤销重做：撤销栈同时还原配方集合（K7）；切换配方不进撤销栈", () => {
      assert.equal(g().createRecipe("A"), true);
      assert.equal(g().createRecipe("a"), false, "重名不分大小写");
      g().createRecipe("B");
      const pastAfterCreate = g().past.length;
      selectRecipe("A");
      assert.equal(g().past.length, pastAfterCreate, "切换配方不产生撤销记录");
      // K6 ①：选着配方改已纳入的参数 → 写进当前配方，default 不动
      g().setParam("n_voxel", "leafSize", [0.03, 0.03, 0.03]);
      assert.deepEqual(g().doc.params.leafSize.default, [0.005, 0.005, 0.005]);
      assert.deepEqual(r().set.recipes[0].values, { leafSize: [0.03, 0.03, 0.03] });
      assert.deepEqual(runParamsOf(g().doc).leafSize, [0.03, 0.03, 0.03]);
      assert.equal(g().past.at(-1).label, "配方 A：修改 leafSize");
      assert.equal(recipesDirty(), true);

      g().undo();
      assert.deepEqual(r().set.recipes[0].values, {});
      assert.equal(r().current, "A", "撤销不改当前配方");
      g().redo();
      assert.deepEqual(r().set.recipes[0].values, { leafSize: [0.03, 0.03, 0.03] });
      g().undo();
      g().undo();
      g().undo();
      assert.equal(r().set.recipes.length, 0);
      assert.equal(r().current, null, "配方被撤没了就回到基础");
      assert.equal(recipesDirty(), false, "撤销回到载入时的那一份：不脏");
    }],
    ["K4：配方 A 覆盖 x、B 覆盖 y，A → B 之后 x 回到基础，不残留 A 的值", () => {
      g().createRecipe("A");
      g().createRecipe("B");
      g().setRecipeValue("A", "leafSize", [0.04, 0.04, 0.04]);
      g().setRecipeValue("B", "planeTol", 0.01);
      selectRecipe("A");
      assert.deepEqual(runParamsOf(g().doc), { planeTol: 0.006, leafSize: [0.04, 0.04, 0.04] });
      selectRecipe("B");
      assert.deepEqual(runParamsOf(g().doc), { planeTol: 0.01, leafSize: [0.005, 0.005, 0.005] });
    }],
    ["管理动作：复制、改名（当前配方跟着走）、设默认、删除（默认一并取消），都能撤销", () => {
      g().createRecipe("A");
      g().setRecipeValue("A", "planeTol", 0.02);
      g().createRecipe("A 副本", "A");
      assert.equal(r().set.recipes[1].values.planeTol, 0.02);
      selectRecipe("A");
      g().renameRecipe("A", "车型A·左前门");
      assert.equal(r().current, "车型A·左前门");
      g().setDefaultRecipe("车型A·左前门");
      assert.equal(r().set.defaultName, "车型A·左前门");
      g().deleteRecipe("车型A·左前门");
      assert.deepEqual(r().set.recipes.map((e) => e.name), ["A 副本"]);
      assert.equal(r().set.defaultName, null);
      assert.equal(r().current, null);
      g().undo();
      assert.equal(r().set.defaultName, "车型A·左前门");
      g().undo();
      g().undo();
      assert.deepEqual(r().set.recipes.map((e) => e.name), ["A", "A 副本"]);
      resetRecipes(null, "none");
      assert.equal(g().createRecipe("X"), false, "没存过盘的图不能建配方");
    }]
  ], setup);
});

test("基础与配方编辑路由：恢复、写回基础和基础改动转为当前配方", () => {
  verifyCases([
    ["恢复基础与写回基础：各一步撤销；写回基础改 default、删覆盖，别的配方跟着新的基础", () => {
      g().createRecipe("A");
      g().createRecipe("B");
      g().setRecipeValue("A", "planeTol", 0.02);
      g().clearRecipeValue("A", "planeTol");
      assert.deepEqual(r().set.recipes[0].values, {});
      g().undo();
      assert.equal(r().set.recipes[0].values.planeTol, 0.02);
      g().writeRecipeValueToBase("A", "planeTol");
      assert.equal(g().doc.params.planeTol.default, 0.02);
      assert.deepEqual(r().set.recipes[0].values, {});
      selectRecipe("B");
      assert.equal(runParamsOf(g().doc).planeTol, 0.02, "B 沿用基础，看到的是新的 default");
      g().undo();
      assert.equal(g().doc.params.planeTol.default, 0.006);
      assert.equal(r().set.recipes[0].values.planeTol, 0.02, "一次撤销同时还原 default 与配方");
    }],
    ["K6 ②：选着配方改没纳入的参数 → 改的是图并记一笔；「改为只在本配方生效」一步把它纳入、新值只进当前配方", () => {
      g().createRecipe("A");
      selectRecipe("A");
      g().setParam("n_voxel", "minPointsPerVoxel", 5);
      assert.equal(g().doc.nodes.find((n) => n.id === "n_voxel").params.minPointsPerVoxel, 5, "照常改图");
      const edit = r().baseEdits["n_voxel.minPointsPerVoxel"];
      assert.deepEqual({ before: edit.before, after: edit.after, recipe: edit.recipe }, { before: 0, after: 5, recipe: "A" });
      const steps = g().past.length;
      const name = g().moveBaseEditToRecipe("n_voxel.minPointsPerVoxel");
      assert.equal(name, "minPointsPerVoxel");
      assert.equal(g().past.length, steps + 1, "一步");
      assert.equal(g().doc.params.minPointsPerVoxel.default, 0, "default = 改之前的值：别的配方行为不变");
      assert.equal(g().doc.nodes.find((n) => n.id === "n_voxel").params.minPointsPerVoxel, undefined);
      assert.equal(r().set.recipes[0].values.minPointsPerVoxel, 5);
      assert.equal(r().baseEdits["n_voxel.minPointsPerVoxel"], undefined);
      g().undo();
      assert.equal(g().doc.params.minPointsPerVoxel, undefined);
      assert.equal(r().set.recipes[0].values.minPointsPerVoxel, undefined);
      assert.equal(g().doc.nodes.find((n) => n.id === "n_voxel").params.minPointsPerVoxel, 5);
    }]
  ], setup);
});

test("配方参数事务：拖动合并、图参数改名与失配阻断及修复撤销", () => {
  verifyCases([
    ["拖滑块：begin/commit 里改配方值合成一条撤销；图参数改名同步改每个配方里的键", () => {
      g().createRecipe("A");
      selectRecipe("A");
      const steps = g().past.length;
      g().begin();
      for (const v of [0.007, 0.008, 0.009]) g().editGraphParamValue("planeTol", v);
      g().commit("拖动 planeTol");
      assert.equal(g().past.length, steps + 1);
      assert.equal(r().set.recipes[0].values.planeTol, 0.009);
      g().undo();
      assert.equal(r().set.recipes[0].values.planeTol, undefined);
      g().redo();
      g().renameGraphParam("planeTol", "binTol");
      assert.deepEqual(Object.keys(r().set.recipes[0].values), ["binTol"]);
      assert.equal(runParamsOf(g().doc).binTol, 0.009);
    }],
    ["失配阻止运行（P3.7）：当前配方有 ①–③ 时给出原因；其余配方与基础不受影响；按建议修复一步撤销", () => {
      g().createRecipe("A");
      g().createRecipe("B");
      g().setRecipeValue("A", "leafSize", [5, 5, 5]); // 越过 max 1
      selectRecipe("A");
      const why = currentRecipeBlocker(g().doc);
      assert.match(why, /配方「A」有 1 处失配/);
      assert.match(why, /leafSize/);
      selectRecipe("B");
      assert.equal(currentRecipeBlocker(g().doc), null);
      selectRecipe(null);
      assert.equal(currentRecipeBlocker(g().doc), null);
      const report = recipeReport(g().doc, r().set.recipes[0]);
      g().fixRecipe("A", report.items);
      assert.deepEqual(r().set.recipes[0].values.leafSize, [1, 1, 1]);
      g().undo();
      assert.deepEqual(r().set.recipes[0].values.leafSize, [5, 5, 5]);
    }]
  ], setup);
});
