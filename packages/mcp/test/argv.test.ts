import assert from "node:assert/strict";
import test from "node:test";

import { evalArgv, paramsArgv, patchArgv, perturbArgv, recipesArgv } from "../src/argv.js";

interface MappingRow {
  name: string;
  argv: string[];
  want: string[];
}

test("工具入参 → CLI argv 的映射（params / recipes / eval / perturb / patch）", () => {
  const rows: MappingRow[] = [
    // ---------------------------------------------------------------- params / recipes
    {
      name: "params 永远带 --json",
      argv: paramsArgv({ graphPath: "g.lyflow.json" }),
      want: ["params", "g.lyflow.json", "--json"],
    },
    {
      name: "params 的过滤项照给",
      argv: paramsArgv({
        graphPath: "g.lyflow.json",
        baseDir: "configs/R1",
        node: ["n_fit_l", "n_fit_r"],
        only: "explicit",
        set: ["n_fit_l.distThresh=0.8"],
      }),
      want: [
        "params", "g.lyflow.json",
        "--base-dir", "configs/R1",
        "--node", "n_fit_l",
        "--node", "n_fit_r",
        "--only", "explicit",
        "--set", "n_fit_l.distThresh=0.8",
        "--json",
      ],
    },
    {
      name: "params 的 recipe 透传成 --recipe（param-recipe P4.2）",
      argv: paramsArgv({ graphPath: "g.lyflow.json", recipe: "g.recipes/A.lyflow-recipe.json", only: "graph" }),
      want: ["params", "g.lyflow.json", "--only", "graph", "--recipe", "g.recipes/A.lyflow-recipe.json", "--json"],
    },
    {
      name: "list_recipes 用 recipes --json",
      argv: recipesArgv("g.lyflow.json"),
      want: ["recipes", "g.lyflow.json", "--json"],
    },
    {
      name: "run_graph 带 recipe 时用 recipes --recipe --json",
      argv: recipesArgv("g.lyflow.json", "B.lyflow-recipe.json"),
      want: ["recipes", "g.lyflow.json", "--recipe", "B.lyflow-recipe.json", "--json"],
    },
    // ---------------------------------------------------------------- eval
    {
      // ADR-0022 / m6-plan §10 第 5 条：summary 的体积是逐行的
      name: "eval 最小入参：summary、recipe 默认都不给",
      argv: evalArgv({ graphPath: "g", metric: ["outputs.gap"] }, null),
      want: ["eval", "g", "--metric", "outputs.gap"],
    },
    {
      name: "eval 的 summary:true 才带 --summary",
      argv: evalArgv({ graphPath: "g", metric: ["outputs.gap"], summary: true }, null),
      want: ["eval", "g", "--metric", "outputs.gap", "--summary"],
    },
    {
      name: "eval 的 recipe 透传成 --recipe",
      argv: evalArgv({ graphPath: "g", metric: ["outputs.gap"], recipe: "A.lyflow-recipe.json" }, null),
      want: ["eval", "g", "--metric", "outputs.gap", "--recipe", "A.lyflow-recipe.json"],
    },
    {
      name: "eval 把多个 metric 与多个 param 都展成重复选项",
      argv: evalArgv(
        {
          graphPath: "g.lyflow.json",
          samplesPath: "frames.jsonl",
          metric: ["outputs.gap", "nodes.n_fit.durationMs"],
          param: ["n_fit.distThresh=0.1:0.4:4", "n_fit.minInliers=10:40:4"],
          holdout: "half=b",
          groupBy: "half",
          baseDir: "configs/R1",
          set: ['n_load.layout="sensor"'],
          noCache: true,
        },
        null,
      ),
      want: [
        "eval", "g.lyflow.json",
        "--base-dir", "configs/R1",
        "--samples", "frames.jsonl",
        "--param", "n_fit.distThresh=0.1:0.4:4",
        "--param", "n_fit.minInliers=10:40:4",
        "--metric", "outputs.gap",
        "--metric", "nodes.n_fit.durationMs",
        "--holdout", "half=b",
        "--group-by", "half",
        "--set", 'n_load.layout="sensor"',
        "--no-cache",
      ],
    },
    {
      name: "eval 的显式参数组走 --params 文件",
      argv: evalArgv({ graphPath: "g.json", metric: ["outputs.gap"] }, "C:/tmp/paramsets.json"),
      want: ["eval", "g.json", "--params", "C:/tmp/paramsets.json", "--metric", "outputs.gap"],
    },
    {
      name: "eval 的 samplesGlob 配 bind",
      argv: evalArgv({ graphPath: "g", metric: ["m"], samplesGlob: "a/*.pcd", bind: "n_load.path" }, null),
      want: ["eval", "g", "--samples-glob", "a/*.pcd", "--bind", "n_load.path", "--metric", "m"],
    },
    {
      name: "eval 的 samplesDir 把配对、子目录、排序与打 tag 都映射成 CLI 选项",
      argv: evalArgv(
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
      ),
      want: [
        "eval", "4.lyflow.json",
        "--samples-dir", "D:/data/sensor",
        "--bind-pair", "n_load.primaryFile,n_load.secondaryFile",
        "--pattern", "*Master*.pcd,*Slave*.pcd",
        "--sample-subdir", "4",
        "--sort-by", "mtime",
        "--split-half", "half",
        "--metric", "outputs.gap",
        "--holdout", "half=b",
        "--csv", "D:/tmp/p4.csv",
      ],
    },
    {
      name: "samplesDir 的单文件写法用 bind，不用 bindPair",
      argv: evalArgv({ graphPath: "g", samplesDir: "D:/data", bind: "r.path", pattern: "*.pcd", metric: ["m"] }, null),
      want: ["eval", "g", "--samples-dir", "D:/data", "--bind", "r.path", "--pattern", "*.pcd", "--metric", "m"],
    },
    // ---------------------------------------------------------------- perturb
    {
      name: "perturb 把 region 序列化成一个 JSON 参数",
      argv: perturbArgv({
        graphPath: "g.lyflow.json",
        after: "n_frame_s:cloud",
        region: { kind: "halfspace", point: [0.01345, 0, 0], normal: [1, 0, 0] },
        axis: "x=-0.0003:0.0003:5",
        samplesPath: "frames.jsonl",
        metric: ["outputs.gap"],
        expect: 1000,
        tolerance: 100,
      }),
      want: [
        "perturb", "g.lyflow.json",
        "--after", "n_frame_s:cloud",
        "--region", '{"kind":"halfspace","point":[0.01345,0,0],"normal":[1,0,0]}',
        "--axis", "x=-0.0003:0.0003:5",
        "--samples", "frames.jsonl",
        "--metric", "outputs.gap",
        "--expect", "1000",
        "--tolerance", "100",
      ],
    },
    {
      name: "perturb 的 expect=0 也要传下去（0 是假值，不能被当成没给）",
      argv: perturbArgv({
        graphPath: "g",
        after: "n:cloud",
        region: { kind: "box", min: [0, 0, 0], max: [1, 1, 1] },
        axis: "z=0:1:3",
        metric: ["outputs.gap"],
        expect: 0,
      }),
      want: [
        "perturb", "g",
        "--after", "n:cloud",
        "--region", '{"kind":"box","min":[0,0,0],"max":[1,1,1]}',
        "--axis", "z=0:1:3",
        "--metric", "outputs.gap",
        "--expect", "0",
      ],
    },
    {
      name: "perturb 也认 samplesDir 那一组，csv 与 noCache 一并透传",
      argv: perturbArgv({
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
      }),
      want: [
        "perturb", "g",
        "--after", "n_frame_p:cloud",
        "--region", '{"kind":"halfspace","point":[0.0098,0,0],"normal":[-1,0,0]}',
        "--axis", "x=-0.0003:0.0003:5",
        "--samples-dir", "D:/data/sensor",
        "--bind-pair", "n_load.primaryFile,n_load.secondaryFile",
        "--pattern", "*Master*.pcd,*Slave*.pcd",
        "--sample-subdir", "Audio_1",
        "--metric", "outputs.gap",
        "--expect", "-1000",
        "--csv", "D:/tmp/a1.csv",
        "--no-cache",
      ],
    },
    // ---------------------------------------------------------------- patch
    {
      name: "patch 把四个动作按 remove → add → rewire → set 的顺序展开，dryRun 默认开",
      argv: patchArgv({
        graphPath: "4.lyflow.json",
        baseDir: "configs/R1",
        removeNode: ["b_*", "n_dead"],
        addNode: [{ id: "g2", op: "gen.synthetic", params: { seed: 3 } }],
        rewire: ["n_fb_line:out=n_fit_base:line"],
        set: ["n_fit_l.distThresh=0.8"],
      }),
      want: [
        "patch", "4.lyflow.json",
        "--base-dir", "configs/R1",
        "--remove-node", "b_*",
        "--remove-node", "n_dead",
        "--add-node", '{"id":"g2","op":"gen.synthetic","params":{"seed":3}}',
        "--rewire", "n_fb_line:out=n_fit_base:line",
        "--set", "n_fit_l.distThresh=0.8",
        "--dry-run",
        "--json",
      ],
    },
    {
      name: "patch 的 dryRun:false 才真写，out 配着它给",
      argv: patchArgv({ graphPath: "g.json", removeNode: ["b_*"], dryRun: false, out: "out.json" }),
      want: ["patch", "g.json", "--remove-node", "b_*", "-o", "out.json", "--json"],
    },
  ];
  for (const { name, argv, want } of rows) assert.deepEqual(argv, want, name);
});

test("入参不成立时当场报错，不去起 CLI", () => {
  const rows: [name: string, call: () => unknown, error: RegExp][] = [
    ["eval 至少要一个 metric", () => evalArgv({ graphPath: "g", metric: [] }, null), /metric/],
    ["samplesGlob 必须配 bind", () => evalArgv({ graphPath: "g", metric: ["m"], samplesGlob: "*.pcd" }, null), /bind/],
    [
      "samplesGlob 不能与 samplesPath 同时给",
      () => evalArgv({ graphPath: "g", metric: ["m"], samplesGlob: "*.pcd", samplesPath: "s.jsonl" }, null),
      /只能给一个/,
    ],
    [
      "samplesDir 缺 pattern",
      () => evalArgv({ graphPath: "g", metric: ["m"], samplesDir: "D:/d", bindPair: "a.b,a.c" }, null),
      /pattern/,
    ],
    ["samplesDir 缺绑定", () => evalArgv({ graphPath: "g", metric: ["m"], samplesDir: "D:/d", pattern: "*.pcd" }, null), /bindPair/],
    [
      "samplesDir 不能与 samplesPath 同时给",
      () => evalArgv({ graphPath: "g", metric: ["m"], samplesDir: "D:/d", samplesPath: "s.jsonl" }, null),
      /只能给一个/,
    ],
    [
      "splitHalf 要和 samplesDir 一起给",
      () => evalArgv({ graphPath: "g", metric: ["m"], samplesPath: "s.jsonl", splitHalf: "half" }, null),
      /samplesDir/,
    ],
    [
      "perturb 的 after 要写成 节点:端口",
      () =>
        perturbArgv({ graphPath: "g", after: "n_frame_s", region: { kind: "box" }, axis: "x=0:1:2", metric: ["outputs.gap"] }),
      /<nodeId>:<port>/,
    ],
    ["patch 至少要一个动作", () => patchArgv({ graphPath: "g.json" }), /至少给一个动作/],
    ["patch 的 out 要配 dryRun:false", () => patchArgv({ graphPath: "g.json", removeNode: ["b_*"], out: "out.json" }), /dryRun:false/],
    ["rewire 两端都没写端口", () => patchArgv({ graphPath: "g.json", rewire: ["n_fb_line=n_fit"] }), /端口/],
    ["rewire 目标端没写端口", () => patchArgv({ graphPath: "g.json", rewire: ["a:out=b"] }), /端口/],
  ];
  for (const [name, call, error] of rows) assert.throws(call, error, name);
});
