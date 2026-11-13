// param-recipe P2.6：transform 与 curve 两种值的纯函数（lib/transform.ts、lib/curve.ts）。
// 判据与 core 的 checkCurveValue / evaluateCurve、transform.make 的旋转约定逐条对应。
import assert from "node:assert/strict";
import { test } from "node:test";

import { paramFacts } from "../src/lib/paramDoc.ts";

import {
  asCurve,
  curveProblem,
  evaluate,
  insertAt,
  insertInWidestGap,
  movePoint,
  normalized,
  removePoint,
  yRange,
} from "../src/lib/curve.ts";
import { niceStep, stepFor, valueEquals } from "../src/lib/params.ts";
import { asMatrix, compose, decompose, IDENTITY, isRigid, summarize } from "../src/lib/transform.ts";

const close = (a, b, eps = 1e-9) => Math.abs(a - b) <= eps;

test("transform：T·R 行主序往返、万向锁、非刚体和坏值回退", () => {
  const axis = compose({ t: [0.1, -0.2, 0.3], r: [0, 0, 90] });
  assert.deepEqual([axis.length, axis[3], axis[7], axis[11], ...axis.slice(12)], [16, 0.1, -0.2, 0.3, 0, 0, 0, 1], "T·R 行主序与平移位置");
  assert.deepEqual([axis[0], axis[4], axis[8]], [0, 1, 0], "绕 Z 转 90°：x 轴转向 y 轴");
  const rx = compose({ t: [0, 0, 0], r: [90, 0, 0] });
  assert.deepEqual([rx[1], rx[5], rx[9]], [0, 0, 1], "绕 X 转 90°：y 轴转向 z 轴");
  for (const [name, r, gimbal] of [
    ["三个旋转分量", [10, 20, 30]],
    ["负角与大角", [-45, 5, 170]],
    ["负俯仰", [0, -60, -120]],
    ["小数角", [33.3, 12.5, -7.25]],
    ["万向锁 ry=90°", [30, 90, 10], true],
  ]) {
    const matrix = compose({ t: [1, 2, 3], r });
    assert.ok(isRigid(matrix), name);
    const back = decompose(matrix);
    assert.deepEqual(back.t, [1, 2, 3], name);
    if (gimbal) {
      assert.equal(back.r[0], 0, name);
      assert.ok(close(back.r[1], 90, 1e-7), name);
    } else assert.ok(back.r.every((v, i) => close(v, r[i], 1e-7)), `${name}：${back.r}`);
    assert.ok(compose(back).every((v, i) => close(v, matrix[i], 1e-9)), `${name}：矩阵往返`);
  }
  const scaled = [...IDENTITY];
  scaled[0] = 2;
  assert.equal(isRigid(scaled), false);
  assert.equal(summarize(scaled), "4×4 矩阵（含缩放或切变）");
  assert.deepEqual(asMatrix([1, 2, 3]), [...IDENTITY]);
  assert.deepEqual(asMatrix(null), [...IDENTITY]);
  assert.equal(summarize(compose({ t: [0.25, 0, -1], r: [0, 0, 45] })), "T[0.25, 0, -1] R[0, 0, 45]°");
});

test("curve：合法性、插值与受限编辑保持曲线及结构相等契约", () => {
  for (const [name, value, want, min, max] of [
    ["线性曲线合法", { points: [[0, 0], [1, 1]] }, null],
    ["平滑曲线合法", { points: [[0, 0], [0.5, 0.2], [1, 1]], interp: "smooth" }, null],
    ["曲线必须是对象", [0, 1], /对象/],
    ["至少两个控制点", { points: [[0, 0]] }, /两个控制点/],
    ["横坐标限定0到1", { points: [[0, 0], [1.2, 1]] }, /\[0, 1\]/],
    ["横坐标严格递增", { points: [[0, 0], [0.5, 1], [0.5, 2]] }, /大于前一个点/],
    ["插值方式非法", { points: [[0, 0], [1, 1]], interp: "cubic" }, /interp/],
    ["张力越界", { points: [[0, 0], [1, 1]], tension: 1 }, /tension/],
    ["纵坐标超出参数上限", { points: [[0, 0], [1, 1.5]] }, /不能大于 1/, 0, 1],
  ]) {
    const problem = curveProblem(value, min, max);
    if (want === null) assert.equal(problem, null, name);
    else assert.match(problem, want, name);
  }
  assert.deepEqual(asCurve("坏的", { points: [[0, 1], [1, 0]] }).points, [[0, 1], [1, 0]]);

  const lin = { points: [[0.2, 1], [0.6, 3]] };
  for (const [name, x, want] of [["线性插值", 0.4, 2], ["左端外取端点", 0, 1], ["右端外取端点", 1, 3]]) {
    assert.equal(evaluate(lin, x), want, name);
  }
  const smooth = { interp: "smooth", points: [[0, 0], [0.5, 0.35], [0.6, 0.95], [1, 1]] };
  let prev = -1;
  for (let i = 0; i <= 100; i += 1) {
    const y = evaluate(smooth, i / 100);
    assert.ok(y >= prev - 1e-12 && y <= 1 + 1e-12, `smooth单调且不冲过：x=${i / 100}`);
    prev = y;
  }
  assert.ok(close(evaluate(smooth, 0.5), 0.35));
  assert.ok(close(evaluate(smooth, 0.6), 0.95));

  const pts = [[0, 0], [0.5, 0.5], [1, 1]];
  assert.deepEqual(movePoint(pts, 1, 2, 3, 0, 1), [[0, 0], [0.999, 1], [1, 1]]);
  assert.deepEqual(movePoint(pts, 0, -1, -1, 0, 1), [[0, 0], [0.5, 0.5], [1, 1]]);
  const c = { points: pts, interp: "linear" };
  const hit = insertAt(c, 0.25);
  assert.equal(hit.index, 1);
  assert.deepEqual(hit.points[1], [0.25, 0.25]);
  assert.equal(insertAt(c, 0.5), null);
  assert.deepEqual(insertInWidestGap({ points: [[0, 0], [0.2, 0.2], [1, 1]] }).points[2], [0.6, 0.6]);
  assert.equal(removePoint([[0, 0], [1, 1]], 0), null);
  assert.deepEqual(removePoint(pts, 1), [[0, 0], [1, 1]]);
  assert.deepEqual(normalized([[0.1 + 0.2, 1]], "smooth"), { points: [[0.3, 1]], interp: "smooth" });
  assert.deepEqual(yRange({ points: [[0, -0.5], [1, 2]] }, {}), [-0.5, 2]);
  assert.deepEqual(yRange({ points: [[0, 0], [1, 1]] }, { softMin: 0, softMax: 10 }), [0, 10]);

  const a = { points: [[0, 0], [1, 1]], interp: "linear" };
  for (const [name, value, equal] of [
    ["对象键书写顺序不影响相等", { interp: "linear", points: [[0, 0], [1, 1]] }, true],
    ["缺少interp不是相同值", { points: [[0, 0], [1, 1]] }, false],
    ["控制点不同", { ...a, points: [[0, 0], [1, 0.9]] }, false],
    ["对象与数组不同", [a], false],
  ]) assert.equal(valueEquals(a, value), equal, name);
});

// 数字框拖一格、按一下 ↑↓ 走多少。修前没范围的一律 0.01、整数一律 1：1e-3 量级的值拖 1 px 就被量化成 0
test("数值编辑：步长量级、全角算式、相对改法、夹住取整与路径规范化", async () => {
  const P = (extra) => ({ name: "p", type: "float", label: "P", default: 0, ...extra });
  const stepCases = [
    ["声明了 step", P({ step: 0.25 }), false, 3, 0.25],
    ["浮点有范围 0–1：0.005", P({ min: 0, max: 1 }), false, 0.3, 0.005],
    ["soft 范围优先，0–3 的 1/200 修整成 0.01", P({ min: 0, max: 100, softMin: 0, softMax: 3 }), false, 1, 0.01],
    ["整数有范围（点数 1–500000）：2000，不再是 1", P({ min: 1, softMax: 500000 }), true, 40000, 2000],
    ["整数没范围，值 1000：10", P({}), true, 1000, 10],
    ["浮点没范围，值 10：0.1", P({}), false, 10, 0.1],
    ["浮点没范围，值 1e-3：1e-5", P({}), false, 1e-3, 1e-5],
    ["值是 0：看默认值 0.5", P({ default: 0.5 }), false, 0, 0.001],
    ["什么都没有：浮点 0.01", P({}), false, undefined, 0.01],
    ["什么都没有：整数 1", P({}), true, 0, 1],
  ];
  for (const [name, param, integer, ref, want] of stepCases) {
    assert.ok(Math.abs(stepFor(param, integer, ref) - want) < want * 1e-9, `${name}：得到 ${stepFor(param, integer, ref)}`);
  }
  assert.deepEqual([2499.995, 0.015, 0.005, 7, 1].map(niceStep), [2000, 0.01, 0.005, 5, 1]);

  // 数字框里打的字（lib/numExpr）：数、算式、相对改法、输入法的全角符号；夹住与取整要说一声
  const { applyNumEdit, numEditNote, parseNumEdit } = await import("../src/lib/numExpr.ts");
  const F = { integer: false };
  const cases = [
    // [说明, 打的字, 当前值, 限制, 期望（数 / {value, clamped, rounded} / "error" / "empty"）]
    ["算式", "0.01*2", 1, F, 0.02],
    ["括号", "(3+4)/2", 1, F, 3.5],
    ["浮点尾巴抹掉", "0.1+0.2", 1, F, 0.3],
    ["科学计数法", "1e-3", 1, F, 0.001],
    ["点开头", ".5", 1, F, 0.5],
    ["空格", " 2 * 3 ", 1, F, 6],
    ["乘", "*2", 5, F, 10],
    ["除", "/2", 5, F, 2.5],
    ["加", "+=5", 5, F, 10],
    ["减", "-=0.5", 5, F, 4.5],
    ["乘一个算式", "*=(1+1)", 5, F, 10],
    ["负数是绝对值（不是减）", "-0.5", 5, F, -0.5],
    ["全角的乘号与数字", "＊２", 5, F, 10],
    ["全角括号", "（３＋４）／２", 1, F, 3.5],
    ["中文句号当小数点", "1。5", 1, F, 1.5],
    ["减号 U+2212", "−3", 1, F, -3],
    ["越过上限：夹住", "+=5", 8, { integer: false, max: 10 }, { value: 10, clamped: "max", rounded: false }],
    ["整数：四舍五入", "*1.5", 3, { integer: true }, { value: 5, clamped: undefined, rounded: true }],
    ["后面跟着字", "1.5abc", 1, F, "error"],
    ["两个乘号", "2**3", 1, F, "error"],
    ["括号没配上", "(1+2", 1, F, "error"],
    ["除以 0", "/0", 1, F, "error"],
    ["太大", "1e400", 1, F, "error"],
    ["空", "", 1, F, "empty"],
  ];
  const wrong = [];
  for (const [name, text, cur, lim, want] of cases) {
    const edit = parseNumEdit(text);
    let got;
    if (edit.kind === "error" || edit.kind === "empty") got = edit.kind;
    else {
      const r = applyNumEdit(edit, cur, lim);
      got = typeof want === "number" ? r.value : { value: r.value, clamped: r.clamped, rounded: r.rounded ?? false };
    }
    if (JSON.stringify(got) !== JSON.stringify(want)) wrong.push([name, got, want]);
  }
  assert.deepEqual(wrong, [], "说明 / 实际 / 期望");
  assert.equal(numEditNote("Point Count", [{ value: 3, clamped: "max" }, { value: 2 }], { max: 3 }),
    "Point Count：2 个里 1 个超出上限 3，已取上限");
  assert.equal(numEditNote("x", [{ value: 2 }], {}), null);

  const { cleanPathText } = await import("../src/lib/params.ts");
  const paths = [
    // [粘进来的, 存下的]
    ['"D:\\data\\a b.pcd"', "D:\\data\\a b.pcd"],
    ["  'rel/x.pcd'  ", "rel/x.pcd"],
    ["\u201cD:\\中文\\x.pcd\u201d", "D:\\中文\\x.pcd"],
    ['" D:\\x.pcd "', "D:\\x.pcd"],
    ['"D:\\x.pcd\'', '"D:\\x.pcd\''],
    ['D:\\"quoted"\\x.pcd', 'D:\\"quoted"\\x.pcd'],
    ['""', ""],
    ['"', '"'],
    ["  plain.pcd\t", "plain.pcd"],
  ];
  for (const [raw, want] of paths) assert.equal(cleanPathText(raw), want, JSON.stringify(raw));
});

// 换算子（lib/replace）时参数怎么带过去：同名同类型才带；枚举值要在新选项里；越界夹进新范围；等于新默认值删键
test("carryParams：换成别的算子时哪些参数带过去", async () => {
  const { carryParams } = await import("../src/lib/replace.ts");
  const oldOp = { params: [
    { name: "k", type: "float", default: 1 },
    { name: "e", type: "enum", default: "a" },
    { name: "n", type: "int", default: 5 },
    { name: "gone", type: "float", default: 0 },
    { name: "same", type: "float", default: 2 },
  ] };
  const newOp = { params: [
    { name: "k", type: "float", default: 1, min: 0.5, max: 3 },
    { name: "e", type: "enum", default: "a", options: [{ value: "a", label: "A" }, { value: "b", label: "B" }] },
    { name: "n", type: "vec3f", default: [0, 0, 0] },
    { name: "same", type: "float", default: 2 },
  ] };
  const cases = [
    // [说明, 原参数, 期望 { params, dropped, clamped }]
    ["同名同类型：带过去", { k: 2 }, { params: { k: 2 }, dropped: [], clamped: [] }],
    ["越界：夹进新范围", { k: 9 }, { params: { k: 3 }, dropped: [], clamped: ["k"] }],
    ["夹到新默认值：删键", { k: 0.1, same: 2 }, { params: { k: 0.5 }, dropped: [], clamped: ["k"] }],
    ["枚举值在新选项里", { e: "b" }, { params: { e: "b" }, dropped: [], clamped: [] }],
    ["枚举值不在新选项里：不带", { e: "c" }, { params: {}, dropped: ["e"], clamped: [] }],
    ["类型变了：不带", { n: 7 }, { params: {}, dropped: ["n"], clamped: [] }],
    ["新算子没有这个参数：不带", { gone: 1 }, { params: {}, dropped: ["gone"], clamped: [] }],
  ];
  for (const [name, params, want] of cases) assert.deepEqual(carryParams(oldOp, newOp, params), want, name);
  assert.deepEqual(carryParams(undefined, newOp, { n: [1, 2, 3] }).params, { n: [1, 2, 3] }, "旧算子没注册：按形状判");
});

test("参数说明那一行（检查器「参数说明」开着时）：默认值、范围、常用范围、单位、枚举的可选项", () => {
  const P = (over) => ({ name: "p", type: "float", default: 0, ...over });
  const cases = [
    ["数、上下限、单位", P({ default: 0.02, min: 0, max: 1, unit: "m" }), "默认 0.02 m · 范围 0 – 1 m"],
    ["常用范围与范围不同才写", P({ default: 5, min: 0, max: 100, softMin: 0, softMax: 10 }), "默认 5 · 范围 0 – 100 · 常用 0 – 10"],
    ["常用范围与范围一样不重复写", P({ default: 5, min: 0, max: 10, softMin: 0, softMax: 10 }), "默认 5 · 范围 0 – 10"],
    ["只有下限", P({ type: "int", default: 1, min: 0 }), "默认 1 · 范围 ≥ 0"],
    ["向量带单位", P({ type: "vec3f", default: [0.01, 0.01, 0.01], unit: "m" }), "默认 [0.01, 0.01, 0.01] m"],
    ["开关", P({ type: "bool", default: false }), "默认 关"],
    ["枚举写选项的名字", P({ type: "enum", default: "count", options: [{ value: "count", label: "Target Count" }, { value: "ratio", label: "Ratio" }] }),
      "默认 Target Count · 可选 Target Count / Ratio"],
    ["空字符串", P({ type: "path", default: "" }), "默认 空"],
    ["不是数的参数也有单位：单独写", P({ type: "string", default: "x", unit: "mm" }), "默认 x · 单位 mm"],
    ["颜色写 #hex、不带单位", P({ type: "color", default: [1, 0.5, 0] }), "默认 #ff8000"],
    ["flags 写开着的那几项", P({ type: "flags", default: 5, options: [{ value: 1, label: "A" }, { value: 2, label: "B" }, { value: 4, label: "C" }] }),
      "默认 A + C · 可选 A / B / C"],
    ["flags 一项都没开", P({ type: "flags", default: 0, options: [{ value: 1, label: "A" }] }), "默认 无 · 可选 A"],
    ["数与范围同一套有效数字（不截成四位）", P({ type: "int", default: 65535, min: 0, max: 65535 }), "默认 65535 · 范围 0 – 65535"],
    ["六位有效数字的向量", P({ type: "vec3f", default: [0.123456, 1, 2] }), "默认 [0.123456, 1, 2]"],
    ["transform 的单位是平移的", P({ type: "transform", default: [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1], unit: "mm" }),
      "默认 T[0, 0, 0] R[0, 0, 0]° · 平移单位 mm"],
  ];
  for (const [name, param, want] of cases) assert.equal(paramFacts(param), want, name);
  // curve：摘要里的 · 换成逗号（不与这一行分隔各项的撞），上下限管的是 y
  assert.equal(paramFacts(P({ type: "curve", default: { points: [[0, 0], [1, 1]], interp: "linear" }, min: 0, max: 1, softMin: 0, softMax: 0.5 })),
    "默认 2 点，线性 · y 范围 0 – 1 · 画布 y 0 – 0.5");
});
