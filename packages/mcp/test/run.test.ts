import assert from "node:assert/strict";
import test from "node:test";

import { parseDiagnostics, summarizeOutputs, summarizeRun } from "../src/run.js";
import type { ExecutionEvent } from "../src/types.js";

const events: ExecutionEvent[] = [
  { kind: "run_started", runId: "r1", seq: 0, nodeCount: 2 },
  { kind: "node_state", runId: "r1", seq: 1, nodeId: "gen", state: "running" },
  {
    kind: "node_state",
    runId: "r1",
    seq: 2,
    nodeId: "gen",
    state: "done",
    durationMs: 12,
    stats: { elementCount: 24000, outputsAvailable: true },
  },
  {
    kind: "node_state",
    runId: "r1",
    seq: 3,
    nodeId: "lazy",
    state: "skipped",
    stats: { outputsAvailable: false, reason: "not_demanded" },
  },
  {
    kind: "node_state",
    runId: "r1",
    seq: 4,
    nodeId: "fit",
    state: "error",
    errors: [{ phase: "execute", code: "not_enough_points", message: "点太少" }],
  },
  { kind: "run_finished", runId: "r1", seq: 5, status: "error", durationMs: 40 },
];

test("run 摘要每个节点只留最后一次状态", () => {
  const summary = summarizeRun(events);
  assert.equal(summary.status, "error");
  assert.equal(summary.durationMs, 40);
  assert.equal(summary.nodes.length, 3);

  const gen = summary.nodes.find((n) => n.id === "gen");
  assert.deepEqual(gen, {
    id: "gen",
    state: "done",
    outputsAvailable: true,
    durationMs: 12,
    elementCount: 24000,
    errors: [],
  });

  const lazy = summary.nodes.find((n) => n.id === "lazy");
  assert.equal(lazy?.state, "skipped");
  assert.equal(lazy?.outputsAvailable, false);

  const fit = summary.nodes.find((n) => n.id === "fit");
  assert.equal(fit?.errors.length, 1);
  assert.equal(fit?.errors[0]?.code, "not_enough_points");
});

test("图级输出只留名字、端口与值，不留字节", () => {
  const outputs = summarizeOutputs({
    gap: {
      node: "n_gap",
      port: "gap",
      type: "Measurement",
      elementCount: 1,
      byteSize: 0,
      value: { kind: "Measurement", value: 5.7031, unit: "mm" },
    },
    thinned: { node: "voxel", port: "cloud", type: "PointCloud", elementCount: 4321, byteSize: 69136 },
    missing: { node: "n", port: "p", type: "Measurement", elementCount: 0, byteSize: 0, missing: true },
  });
  assert.deepEqual(outputs["gap"], {
    node: "n_gap",
    port: "gap",
    type: "Measurement",
    elementCount: 1,
    value: { kind: "Measurement", value: 5.7031, unit: "mm" },
  });
  assert.equal(outputs["thinned"]?.value, undefined);
  assert.equal(outputs["missing"]?.missing, true);
});

test("校验失败的 400 正文能还原成诊断数组", () => {
  const diagnostics = parseDiagnostics(
    '[{"nodeId":"n1","severity":"error","code":"unknown_op","message":"没有这个算子"}]',
  );
  assert.equal(diagnostics?.length, 1);
  assert.equal(diagnostics?.[0]?.code, "unknown_op");

  const wrapped = parseDiagnostics('{"error":"[{\\"nodeId\\":\\"n1\\",\\"code\\":\\"cycle\\"}]"}');
  assert.equal(wrapped?.[0]?.code, "cycle");

  assert.equal(parseDiagnostics("校验失败，没有执行"), null);
});
