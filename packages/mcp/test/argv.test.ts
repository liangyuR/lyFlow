import assert from "node:assert/strict";
import test from "node:test";

import { evalArgv, paramsArgv, patchArgv, perturbArgv, recipesArgv } from "../src/argv.js";

test("eval 的 summary 默认不给 —— 体积是逐行的（ADR-0022 / m6-plan §10 第 5 条）", () => {
  const off = evalArgv({ graphPath: "g", metric: ["outputs.gap"] }, null);
  assert.equal(off.includes("--summary"), false);
  const on = evalArgv({ graphPath: "g", metric: ["outputs.gap"], summary: true }, null);
  assert.equal(on.includes("--summary"), true);
});

test("params 永远带 --json，过滤项照给", () => {
  assert.deepEqual(paramsArgv({ graphPath: "g.lyflow.json" }), [
    "params",
    "g.lyflow.json",
    "--json",
  ]);
  assert.deepEqual(
    paramsArgv({
      graphPath: "g.lyflow.json",
      baseDir: "configs/R1",
      node: ["n_fit_l", "n_fit_r"],
      only: "explicit",
      set: ["n_fit_l.distThresh=0.8"],
    }),
    [
      "params",
      "g.lyflow.json",
      "--base-dir",
      "configs/R1",
      "--node",
      "n_fit_l",
      "--node",
      "n_fit_r",
      "--only",
      "explicit",
      "--set",
      "n_fit_l.distThresh=0.8",
      "--json",
    ],
  );
});

test("recipe（param-recipe P4.2）：params / eval 透传成 --recipe，list_recipes 与 run_graph 用 recipes --json", () => {
  assert.deepEqual(paramsArgv({ graphPath: "g.lyflow.json", recipe: "g.recipes/A.lyflow-recipe.json", only: "graph" }), [
    "params",
    "g.lyflow.json",
    "--only",
    "graph",
    "--recipe",
    "g.recipes/A.lyflow-recipe.json",
    "--json",
  ]);
  const e = evalArgv({ graphPath: "g", metric: ["outputs.gap"], recipe: "A.lyflow-recipe.json" }, null);
  assert.deepEqual(e.slice(e.indexOf("--recipe"), e.indexOf("--recipe") + 2), ["--recipe", "A.lyflow-recipe.json"]);
  assert.equal(evalArgv({ graphPath: "g", metric: ["outputs.gap"] }, null).includes("--recipe"), false);
  assert.deepEqual(recipesArgv("g.lyflow.json"), ["recipes", "g.lyflow.json", "--json"]);
  assert.deepEqual(recipesArgv("g.lyflow.json", "B.lyflow-recipe.json"), [
    "recipes",
    "g.lyflow.json",
    "--recipe",
    "B.lyflow-recipe.json",
    "--json",
  ]);
});

test("eval 把多个 metric 与多个 param 都展成重复选项", () => {
  const argv = evalArgv(
    {
      graphPath: "g.lyflow.json",
      samplesPath: "frames.jsonl",
      metric: ["outputs.gap", "nodes.n_fit.durationMs"],
      param: ["n_fit.distThresh=0.1:0.4:4", "n_fit.minInliers=10:40:4"],
      holdout: "half=b",
      groupBy: "half",
      baseDir: "configs/R1",
      set: ["n_load.layout=\"sensor\""],
      noCache: true,
    },
    null,
  );
  assert.deepEqual(argv, [
    "eval",
    "g.lyflow.json",
    "--base-dir",
    "configs/R1",
    "--samples",
    "frames.jsonl",
    "--param",
    "n_fit.distThresh=0.1:0.4:4",
    "--param",
    "n_fit.minInliers=10:40:4",
    "--metric",
    "outputs.gap",
    "--metric",
    "nodes.n_fit.durationMs",
    "--holdout",
    "half=b",
    "--group-by",
    "half",
    "--set",
    'n_load.layout="sensor"',
    "--no-cache",
  ]);
});

test("eval 的显式参数组走 --params 文件", () => {
  const argv = evalArgv(
    { graphPath: "g.json", metric: ["outputs.gap"] },
    "C:/tmp/paramsets.json",
  );
  assert.deepEqual(argv, [
    "eval",
    "g.json",
    "--params",
    "C:/tmp/paramsets.json",
    "--metric",
    "outputs.gap",
  ]);
});

test("eval 的 samplesGlob 必须配 bind，且不能与 samplesPath 同时给", () => {
  assert.throws(
    () => evalArgv({ graphPath: "g", metric: ["m"], samplesGlob: "*.pcd" }, null),
    /bind/,
  );
  assert.throws(
    () =>
      evalArgv(
        { graphPath: "g", metric: ["m"], samplesGlob: "*.pcd", samplesPath: "s.jsonl" },
        null,
      ),
    /只能给一个/,
  );
  assert.deepEqual(
    evalArgv(
      { graphPath: "g", metric: ["m"], samplesGlob: "a/*.pcd", bind: "n_load.path" },
      null,
    ),
    ["eval", "g", "--samples-glob", "a/*.pcd", "--bind", "n_load.path", "--metric", "m"],
  );
});

test("eval 至少要一个 metric", () => {
  assert.throws(() => evalArgv({ graphPath: "g", metric: [] }, null), /metric/);
});

test("perturb 把 region 序列化成一个 JSON 参数", () => {
  const argv = perturbArgv({
    graphPath: "g.lyflow.json",
    after: "n_frame_s:cloud",
    region: { kind: "halfspace", point: [0.01345, 0, 0], normal: [1, 0, 0] },
    axis: "x=-0.0003:0.0003:5",
    samplesPath: "frames.jsonl",
    metric: ["outputs.gap"],
    expect: 1000,
    tolerance: 100,
  });
  assert.deepEqual(argv, [
    "perturb",
    "g.lyflow.json",
    "--after",
    "n_frame_s:cloud",
    "--region",
    '{"kind":"halfspace","point":[0.01345,0,0],"normal":[1,0,0]}',
    "--axis",
    "x=-0.0003:0.0003:5",
    "--samples",
    "frames.jsonl",
    "--metric",
    "outputs.gap",
    "--expect",
    "1000",
    "--tolerance",
    "100",
  ]);
});

test("perturb 的 after 要写成 节点:端口", () => {
  assert.throws(
    () =>
      perturbArgv({
        graphPath: "g",
        after: "n_frame_s",
        region: { kind: "box" },
        axis: "x=0:1:2",
        metric: ["outputs.gap"],
      }),
    /<nodeId>:<port>/,
  );
});

test("perturb 的 expect=0 也要传下去", () => {
  const argv = perturbArgv({
    graphPath: "g",
    after: "n:cloud",
    region: { kind: "box", min: [0, 0, 0], max: [1, 1, 1] },
    axis: "z=0:1:3",
    metric: ["outputs.gap"],
    expect: 0,
  });
  assert.ok(argv.includes("--expect"));
  assert.equal(argv[argv.indexOf("--expect") + 1], "0");
});

test("eval 的 samplesDir 把配对、子目录、排序与打 tag 都映射成 CLI 选项", () => {
  const argv = evalArgv(
    {
      graphPath: "4.lyflow.json",
      samplesDir: "D:/data/sensor",
      sampleSubdir: "4",
      bindPair: "n_load.primaryFile,n_load.secondaryFile",
      pattern: "*Master*.pcd,*Slave*.pcd",
      sortBy: "mtime",
      splitHalf: "half",
      metric: ["outputs.gap"],
      holdout: "half=b",
      csv: "D:/tmp/p4.csv",
    },
    null,
  );
  assert.deepEqual(argv, [
    "eval",
    "4.lyflow.json",
    "--samples-dir",
    "D:/data/sensor",
    "--bind-pair",
    "n_load.primaryFile,n_load.secondaryFile",
    "--pattern",
    "*Master*.pcd,*Slave*.pcd",
    "--sample-subdir",
    "4",
    "--sort-by",
    "mtime",
    "--split-half",
    "half",
    "--metric",
    "outputs.gap",
    "--holdout",
    "half=b",
    "--csv",
    "D:/tmp/p4.csv",
  ]);
});

test("samplesDir 的单文件写法用 bind，不用 bindPair", () => {
  assert.deepEqual(
    evalArgv(
      {
        graphPath: "g",
        samplesDir: "D:/data",
        bind: "r.path",
        pattern: "*.pcd",
        metric: ["m"],
      },
      null,
    ),
    ["eval", "g", "--samples-dir", "D:/data", "--bind", "r.path", "--pattern", "*.pcd", "--metric", "m"],
  );
});

test("samplesDir 缺 pattern / 缺绑定 / 与别的样本源同时给都报错", () => {
  assert.throws(
    () => evalArgv({ graphPath: "g", metric: ["m"], samplesDir: "D:/d", bindPair: "a.b,a.c" }, null),
    /pattern/,
  );
  assert.throws(
    () => evalArgv({ graphPath: "g", metric: ["m"], samplesDir: "D:/d", pattern: "*.pcd" }, null),
    /bindPair/,
  );
  assert.throws(
    () =>
      evalArgv(
        { graphPath: "g", metric: ["m"], samplesDir: "D:/d", samplesPath: "s.jsonl" },
        null,
      ),
    /只能给一个/,
  );
  assert.throws(
    () => evalArgv({ graphPath: "g", metric: ["m"], samplesPath: "s.jsonl", splitHalf: "half" }, null),
    /samplesDir/,
  );
});

test("patch 把四个动作按 remove → add → rewire → set 的顺序展开，dryRun 默认开", () => {
  const argv = patchArgv({
    graphPath: "4.lyflow.json",
    baseDir: "configs/R1",
    removeNode: ["b_*", "n_dead"],
    addNode: [{ id: "g2", op: "gen.synthetic", params: { seed: 3 } }],
    rewire: ["n_fb_line:out=n_fit_base:line"],
    set: ["n_fit_l.distThresh=0.8"],
  });
  assert.deepEqual(argv, [
    "patch",
    "4.lyflow.json",
    "--base-dir",
    "configs/R1",
    "--remove-node",
    "b_*",
    "--remove-node",
    "n_dead",
    "--add-node",
    '{"id":"g2","op":"gen.synthetic","params":{"seed":3}}',
    "--rewire",
    "n_fb_line:out=n_fit_base:line",
    "--set",
    "n_fit_l.distThresh=0.8",
    "--dry-run",
    "--json",
  ]);
});

test("patch 的 dryRun:false 才真写，out 要配着它给", () => {
  assert.deepEqual(
    patchArgv({ graphPath: "g.json", removeNode: ["b_*"], dryRun: false, out: "out.json" }),
    ["patch", "g.json", "--remove-node", "b_*", "-o", "out.json", "--json"],
  );
  assert.ok(!patchArgv({ graphPath: "g.json", set: ["a.b=1"], dryRun: false }).includes("--dry-run"));
  assert.throws(
    () => patchArgv({ graphPath: "g.json", removeNode: ["b_*"], out: "out.json" }),
    /dryRun:false/,
  );
});

test("patch 至少要一个动作，rewire 两端都要写成 节点:端口", () => {
  assert.throws(() => patchArgv({ graphPath: "g.json" }), /至少给一个动作/);
  assert.throws(() => patchArgv({ graphPath: "g.json", rewire: ["n_fb_line=n_fit"] }), /端口/);
  assert.throws(() => patchArgv({ graphPath: "g.json", rewire: ["a:out=b"] }), /端口/);
});

test("perturb 也认 samplesDir 那一组，csv 与 noCache 一并透传", () => {
  const argv = perturbArgv({
    graphPath: "g",
    after: "n_frame_p:cloud",
    region: { kind: "halfspace", point: [0.0098, 0, 0], normal: [-1, 0, 0] },
    axis: "x=-0.0003:0.0003:5",
    samplesDir: "D:/data/sensor",
    sampleSubdir: "Audio_1",
    bindPair: "n_load.primaryFile,n_load.secondaryFile",
    pattern: "*Master*.pcd,*Slave*.pcd",
    metric: ["outputs.gap"],
    expect: -1000,
    csv: "D:/tmp/a1.csv",
    noCache: true,
  });
  assert.deepEqual(argv.slice(argv.indexOf("--samples-dir")), [
    "--samples-dir",
    "D:/data/sensor",
    "--bind-pair",
    "n_load.primaryFile,n_load.secondaryFile",
    "--pattern",
    "*Master*.pcd,*Slave*.pcd",
    "--sample-subdir",
    "Audio_1",
    "--metric",
    "outputs.gap",
    "--expect",
    "-1000",
    "--csv",
    "D:/tmp/a1.csv",
    "--no-cache",
  ]);
});
