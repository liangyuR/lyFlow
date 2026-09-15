import assert from "node:assert/strict";
import test from "node:test";

import { evalArgv, perturbArgv } from "../src/argv.js";

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
