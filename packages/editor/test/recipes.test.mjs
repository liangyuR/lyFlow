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

test("sha256：标准测试向量、跨块的长输入、中文（UTF-8 字节）与 node:crypto 逐字节相同", () => {
  assert.equal(sha256Hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  assert.equal(sha256Hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  for (const s of ["x".repeat(55), "x".repeat(56), "x".repeat(64), "车型A·左前门".repeat(40), "𝒳 emoji 😀"]) {
    assert.equal(sha256Hex(s), createHash("sha256").update(s, "utf8").digest("hex"), JSON.stringify(s.slice(0, 20)));
  }
});

// ------------------------------------------------------------ specDigest 的规范化

const gp = (extra = {}) => ({ default: 1, binds: ["n.a"], ...extra });

test("specDigest：与共享夹具里记着的规范形和摘要一致（P4 的 Rust 实现对着同一个数）", () => {
  assert.equal(specCanonical(fixtureDoc), expected.specCanonical);
  assert.equal(specDigest(fixtureDoc), expected.specDigest);
  assert.match(specDigest(fixtureDoc), /^sha256:[0-9a-f]{64}$/);
});

test("specDigest：与图参数的书写顺序、label / 单位 / group / 软限位 / default / binds 无关", () => {
  const a = { params: { x: gp({ type: "float", min: 0, max: 1 }), y: gp({ type: "int" }) } };
  const reordered = { params: { y: gp({ type: "int" }), x: gp({ type: "float", min: 0, max: 1 }) } };
  const cosmetic = {
    params: {
      x: gp({ type: "float", min: 0, max: 1, label: "别的名字", unit: "mm", group: "g", softMin: 0.2, softMax: 0.8, step: 0.1, default: 0.5, binds: ["m.b", "k.c"], doc: "…" }),
      y: gp({ type: "int", advanced: true }),
    },
  };
  assert.equal(specDigest(a), specDigest(reordered));
  assert.equal(specDigest(a), specDigest(cosmetic));
});

test("specDigest：名字、类型、min、max、options 任何一项变了摘要就变；options 与顺序无关、只看 value", () => {
  const base = { params: { m: gp({ type: "enum", options: [{ value: "a", label: "A" }, { value: "b", label: "B" }] }) } };
  const d0 = specDigest(base);
  assert.equal(specDigest({ params: { m: gp({ type: "enum", options: [{ value: "b", label: "乙" }, { value: "a", label: "甲" }] }) } }), d0);
  assert.notEqual(specDigest({ params: { m: gp({ type: "enum", options: [{ value: "a", label: "A" }] }) } }), d0);
  assert.notEqual(specDigest({ params: { n: gp({ type: "enum", options: [{ value: "a", label: "A" }, { value: "b", label: "B" }] }) } }), d0);
  const f = (extra) => specDigest({ params: { x: gp({ type: "float", ...extra }) } });
  assert.notEqual(f({ min: 0 }), f({}));
  assert.notEqual(f({ max: 1 }), f({ max: 2 }));
  assert.notEqual(f({}), specDigest({ params: { x: gp({ type: "int" }) } }));
  // 没有 type 的老格式：只有名字算数（min / max 在 core 里本来就不算规格）
  assert.equal(specDigest({ params: { x: gp({ max: 5 }) } }), specDigest({ params: { x: gp({}) } }));
  assert.notEqual(specDigest({ params: { x: gp({}) } }), specDigest({ params: { x: gp({ type: "float" }) } }));
  // 没有图参数也有一个确定的摘要（空数组）
  assert.equal(specCanonical({}), "[]");
});

test("specDigest：名字按码点序排（BMP 外的字符也与 Rust 的字节序一致），数用 ECMAScript 的格式", () => {
  // U+FF5E（BMP 内，UTF-16 码元 0xFF5E）与 U+1F600（BMP 外，码元 0xD83D…）：码元序里后者在前，码点序里在后
  const doc = { params: { "\u{1F600}": gp({ type: "float" }), "\uFF5E": gp({ type: "float", min: 0.1, max: 1e21 }) } };
  const rows = JSON.parse(specCanonical(doc));
  assert.deepEqual(rows.map((r) => r[0]), ["\uFF5E", "\u{1F600}"]);
  assert.ok(specCanonical(doc).includes("0.1,1e+21"), specCanonical(doc));
});

// ------------------------------------------------------------ 四类失配：共享夹具

test("失配报告：graph.recipes/ 里每个文件的条目（类别、参数、建议、文案）与 expected.json 逐条相同", () => {
  const files = readdirSync(new URL(`${FIX}graph.recipes/`, root)).filter((f) => f.endsWith(".lyflow-recipe.json"));
  assert.deepEqual(files.sort(), Object.keys(expected.recipes).sort(), "夹具目录与 expected.json 列的文件对不上");
  for (const file of files) {
    const parsed = parseRecipeText(readText(`${FIX}graph.recipes/${file}`), file.replace(".lyflow-recipe.json", ""));
    assert.ok(!("error" in parsed), file);
    const report = recipeReport(fixtureDoc, parsed);
    const want = expected.recipes[file];
    assert.equal(report.blocking, want.blocking, `${file} 的 blocking`);
    // message / fixLabel 也在夹具里：P4 的 Rust 实现（bridge/src/recipe.rs）对着同一份逐字比，CLI 的 stderr 与编辑器同一套用语
    assert.deepEqual(
      report.items.map((m) => ({ kind: m.kind, param: m.param, fix: m.fix, message: m.message, fixLabel: m.fixLabel })),
      want.items,
      `${file} 的条目`,
    );
    for (const m of report.items) assert.ok(m.message && m.fixLabel, `${file} ${m.param} 缺文案`);
  }
});

test("失配：constructor、toString、__proto__ 这类名字也是「多出」（只认 doc.params 自己的键，docs/recipe.md ①）", () => {
  const values = JSON.parse('{"constructor": 1, "toString": 2, "__proto__": 3}');
  const r = recipeReport(fixtureDoc, { values, graph: graphRefOf(fixtureDoc) });
  assert.deepEqual(
    r.items.map((m) => [m.kind, m.param]),
    [["extra", "__proto__"], ["extra", "constructor"], ["extra", "toString"]],
  );
  assert.equal(r.blocking, 3);
});

test("失配修复：按建议全部修完后 ①–③ 清零、④ 也消失（记成当前图），并且只改了报告里那几个值", () => {
  for (const file of Object.keys(expected.recipes)) {
    const parsed = parseRecipeText(readText(`${FIX}graph.recipes/${file}`), file);
    const entry = { id: "t", name: file, values: parsed.values, graph: parsed.graph, updatedAt: null };
    const before = recipeReport(fixtureDoc, entry);
    const fixed = applyFixes(entry, before.items, fixtureDoc, "2026-09-24T00:00:00.000Z");
    const after = recipeReport(fixtureDoc, fixed);
    assert.equal(after.items.length, 0, `${file}：${JSON.stringify(after.items)}`);
    if (before.items.length === 0) assert.equal(fixed, entry, `${file}：没失配就不该改`);
    else assert.deepEqual(fixed.graph, graphRefOf(fixtureDoc));
    const touched = new Set(before.items.map((m) => m.param).filter(Boolean));
    for (const [k, v] of Object.entries(entry.values)) {
      if (!touched.has(k)) assert.deepEqual(fixed.values[k], v, `${file}：${k} 不在报告里却被改了`);
    }
  }
});

test("失配：没有 type 的老格式图参数不查类型与限位；缺失的值不算失配（稀疏，用基础）", () => {
  const r = recipeReport(fixtureDoc, { values: { legacyMin: { anything: true } }, graph: graphRefOf(fixtureDoc) });
  assert.equal(r.items.length, 0);
  assert.equal(recipeReport(fixtureDoc, { values: {}, graph: graphRefOf(fixtureDoc) }).items.length, 0);
});

// ------------------------------------------------------------ 文件格式、目录、名字

test("配方文件：只存与基础不同的值、values 按图参数顺序排、读回来值不变；index.json 往返", () => {
  let entry = { id: "a", name: "车型A·左前门", values: {}, graph: graphRefOf(fixtureDoc), updatedAt: "2026-09-24T01:02:03.000Z" };
  entry = withValue(entry, fixtureDoc, "cutMax", 2.5);
  entry = withValue(entry, fixtureDoc, "leafSize", [0.03, 0.03, 0.03]);
  entry = withValue(entry, fixtureDoc, "tint", [0.29, 0.62, 1]); // 等于基础：不存
  assert.deepEqual(Object.keys(entry.values), ["cutMax", "leafSize"]);
  assert.equal(withValue(entry, fixtureDoc, "cutMax", 1.2).values.cutMax, undefined, "写回基础值 = 删掉覆盖");
  const text = serializeRecipe(entry, fixtureDoc);
  assert.ok(text.endsWith("\n"));
  const raw = JSON.parse(text);
  assert.deepEqual(Object.keys(raw), ["schemaVersion", "name", "graph", "values", "updatedAt"]);
  assert.deepEqual(Object.keys(raw.values), ["leafSize", "cutMax"], "按 doc.params 的顺序");
  assert.equal(raw.graph.specDigest, expected.specDigest);
  const back = parseRecipeText(text, "别的名字");
  assert.equal(back.name, "车型A·左前门");
  assert.deepEqual(back.values, entry.values);
  assert.deepEqual(back.notes, []);

  const set = { recipes: [entry, { ...entry, id: "b", name: "车型B" }], defaultName: "车型B" };
  assert.deepEqual(JSON.parse(serializeIndex(set)), { default: "车型B", order: ["车型A·左前门", "车型B"] });
  assert.deepEqual(parseIndex(serializeIndex(set)), { defaultName: "车型B", order: ["车型A·左前门", "车型B"] });
  assert.deepEqual(parseIndex("坏的"), { defaultName: null, order: [] });
});

test("schema：编辑器写出的配方文件满足 schema/recipe.schema.json 的字段、必填、模式；index.json 满足 recipe-index 的", () => {
  const schema = readJson("schema/recipe.schema.json");
  const indexSchema = readJson("schema/recipe-index.schema.json");
  const entry = withValue(
    { id: "a", name: "车型A·左前门", values: {}, graph: graphRefOf(fixtureDoc), updatedAt: "2026-09-24T01:02:03.000Z", note: "备注" },
    fixtureDoc,
    "cutMax",
    2.5,
  );
  const raw = JSON.parse(serializeRecipe(entry, fixtureDoc));
  for (const k of schema.required) assert.ok(k in raw, `缺必填字段 ${k}`);
  for (const k of Object.keys(raw)) assert.ok(k in schema.properties, `schema 不认识的字段 ${k}`);
  assert.equal(raw.schemaVersion, schema.properties.schemaVersion.const);
  assert.match(raw.name, new RegExp(schema.properties.name.pattern));
  assert.match(raw.graph.specDigest, new RegExp(schema.properties.graph.properties.specDigest.pattern));
  assert.deepEqual(Object.keys(raw.graph).sort(), [...schema.properties.graph.required].sort());
  const keyPattern = new RegExp(schema.properties.values.propertyNames.pattern);
  for (const k of Object.keys(raw.values)) assert.match(k, keyPattern);
  assert.ok(!Number.isNaN(Date.parse(raw.updatedAt)));
  // 名字的规矩比 schema 严（还有保留名、首尾空格、「基础」），但 schema 拒的编辑器一定拒
  const namePattern = new RegExp(schema.properties.name.pattern);
  for (const bad of ["a/b", "a\\b", "a:b", "a*b", "a?b", 'a"b', "a<b", "a>b", "a|b", "a\u0001b"]) {
    assert.ok(!namePattern.test(bad), `schema 该拒 ${JSON.stringify(bad)}`);
    assert.ok(recipeNameProblem(EMPTY_SET, bad), `编辑器该拒 ${JSON.stringify(bad)}`);
  }
  const index = JSON.parse(serializeIndex({ recipes: [entry], defaultName: entry.name }));
  for (const k of Object.keys(index)) assert.ok(k in indexSchema.properties, `index.json 多了 ${k}`);
});

test("配方文件：读不出来的给原因；缺字段、多字段照读并记一笔", () => {
  assert.ok("error" in parseRecipeText("{", "x"));
  assert.ok("error" in parseRecipeText("[1]", "x"));
  assert.ok("error" in parseRecipeText('{"schemaVersion": 9}', "x"));
  const loose = parseRecipeText('{"values": {"a": 1}, "extra": 1}', "文件名");
  assert.equal(loose.name, "文件名");
  assert.equal(loose.graph, null);
  assert.equal(loose.notes.length, 3);
});

test("配方目录：图文件同目录的 <图文件名去扩展名>.recipes/，分隔符跟着图路径走", () => {
  assert.equal(recipeDirOf("C:\\工程\\车门缝隙.lyflow.json"), "C:\\工程\\车门缝隙.recipes");
  assert.equal(recipeDirOf("graphs/demo.LYFLOW.json"), "graphs/demo.recipes");
  assert.equal(recipeDirOf("demo.json"), "demo.recipes");
  assert.equal(recipeDirOf("/srv/a.b/g"), "/srv/a.b/g.recipes");
});

test("配方名：按 Windows 文件名的规矩、重名不分大小写、不能叫「基础」", () => {
  const set = { recipes: [{ id: "1", name: "车型A", values: {}, graph: null, updatedAt: null }], defaultName: null };
  assert.equal(recipeNameProblem(set, "车型B·右前门"), null);
  assert.ok(recipeNameProblem(set, "车型a"));
  assert.equal(recipeNameProblem(set, "车型A", "车型A"), null, "改名时跟自己不算重名");
  for (const bad of ["", " a", "a/b", "a:b", "a?", "CON", "lpt1", "a.", "基础"]) {
    assert.ok(recipeNameProblem(EMPTY_SET, bad), JSON.stringify(bad));
  }
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

test("新建配方、写值、撤销重做：撤销栈同时还原配方集合（K7）；切换配方不进撤销栈", () => {
  setup();
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
});

test("K4：配方 A 覆盖 x、B 覆盖 y，A → B 之后 x 回到基础，不残留 A 的值", () => {
  setup();
  g().createRecipe("A");
  g().createRecipe("B");
  g().setRecipeValue("A", "leafSize", [0.04, 0.04, 0.04]);
  g().setRecipeValue("B", "planeTol", 0.01);
  selectRecipe("A");
  assert.deepEqual(runParamsOf(g().doc), { planeTol: 0.006, leafSize: [0.04, 0.04, 0.04] });
  selectRecipe("B");
  assert.deepEqual(runParamsOf(g().doc), { planeTol: 0.01, leafSize: [0.005, 0.005, 0.005] });
});

test("恢复基础与写回基础：各一步撤销；写回基础改 default、删覆盖，别的配方跟着新的基础", () => {
  setup();
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
});

test("K6 ②：选着配方改没纳入的参数 → 改的是图并记一笔；「改为只在本配方生效」一步把它纳入、新值只进当前配方", () => {
  setup();
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
});

test("拖滑块：begin/commit 里改配方值合成一条撤销；图参数改名同步改每个配方里的键", () => {
  setup();
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
});

test("失配阻止运行（P3.7）：当前配方有 ①–③ 时给出原因；其余配方与基础不受影响；按建议修复一步撤销", () => {
  setup();
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
});

test("管理动作：复制、改名（当前配方跟着走）、设默认、删除（默认一并取消），都能撤销", () => {
  setup();
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
});
