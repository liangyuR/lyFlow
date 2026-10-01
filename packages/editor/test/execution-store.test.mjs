// 执行 store 的「节点表反映哪一次运行」（store/execution 的 resultRunId，ADR-0027）：带 targets 的运行可能在桥接层排队，
// 开跑（run_started）之前视图照旧按上一次取；排队的那次没开跑就收场时，被抢占的那次留下的半截状态落成「已取消」。
// 真排队、真抢占由 bridge/src/execution.rs 的单测与 e2e noderun.mjs 的抢占组走；这里只钉 store 怎么接事件。
import assert from "node:assert/strict";
import { test } from "node:test";

import { onNodeTransition, useExecutionStore } from "../src/store/execution.ts";

let seqs = new Map();
const ev = (runId, kind, rest = {}) => {
  const seq = seqs.get(runId) ?? 0;
  seqs.set(runId, seq + 1);
  return { schemaVersion: 1, runId, seq, kind, ...rest };
};
const apply = (e) => useExecutionStore.getState().apply(e);
const stateOf = (id) => useExecutionStore.getState().nodes.get(id)?.state ?? null;

/** 一次跑到一半的全图运行 A：n1 算完、n2 正在算 —— 接着发起的运行会抢占它，它后面的事件都认不到新 runId 上。
 *  preview = true：收场时不去刷缓存统计（那是一次 IPC）。node_state 按 16 ms 合并落库（BATCH_MS），
 *  等过这一窗才算「界面上已经是这样」—— beginRun 会扔掉还没落库的那批。 */
async function halfwayRun() {
  useExecutionStore.getState().reset();
  seqs = new Map();
  useExecutionStore.getState().beginRun("A", [], true);
  apply(ev("A", "run_started", { plan: ["n1", "n2"], targets: [], nodes: [] }));
  apply(ev("A", "node_state", { nodeId: "n1", state: "done" }));
  apply(ev("A", "node_state", { nodeId: "n2", state: "running" }));
  await new Promise((resolve) => setTimeout(resolve, 40));
}

test("resultRunId：全图运行发起即换；带 targets 的等它的 run_started", async () => {
  await halfwayRun();
  assert.equal(useExecutionStore.getState().resultRunId, "A", "全图运行 A 发起时就归它");

  const cases = [
    // [说明, beginRun 的参数, 发起后、开跑前的 resultRunId]
    ["预览（targets）", ["B", ["n2"], true], "A"],
    ["单节点运行（isolate）", ["B", [], false, ["n2"]], "A"],
    ["全图运行", ["B", [], true], "B"],
  ];
  for (const [name, args, beforeStart] of cases) {
    await halfwayRun();
    useExecutionStore.getState().beginRun(...args);
    assert.equal(useExecutionStore.getState().runId, "B", name);
    assert.equal(useExecutionStore.getState().resultRunId, beforeStart, `${name}：开跑前`);
    apply(ev("B", "run_started", { plan: ["n2"], targets: args[1], isolate: args[3] ?? [], nodes: [] }));
    assert.equal(useExecutionStore.getState().resultRunId, "B", `${name}：run_started 之后`);
  }
});

test("排队的那次没开跑就收场：视图留在被抢占的那次，它在算的节点落成已取消（不一直转圈）", async () => {
  const seen = [];
  const off = onNodeTransition((t) => seen.push(`${t.nodeId}:${t.state}`));
  try {
    for (const status of ["cancelled", "error"]) {
      await halfwayRun();
      seen.length = 0;
      useExecutionStore.getState().beginRun("B", ["n2"], true);
      // 桥接层替从没开跑的请求补的那一条（ADR-0027）：seq 0、没有 attached
      apply({ schemaVersion: 1, runId: "B", seq: 0, kind: "run_finished", status });
      const s = useExecutionStore.getState();
      assert.deepEqual(
        [s.runStatus, s.resultRunId, stateOf("n1"), stateOf("n2")],
        [status, "A", "done", "cancelled"],
        `合成的 run_finished（${status}）`,
      );
      assert.deepEqual(seen, ["n2:cancelled"], `${status}：流水账里记了这一下`);
    }

    // 对照：开跑过的部分运行收场时不替 core 改状态（它自己的 node_state 说了算）
    await halfwayRun();
    useExecutionStore.getState().beginRun("B", ["n2"], true);
    apply(ev("B", "run_started", { plan: ["n2"], targets: ["n2"], nodes: [] }));
    apply(ev("B", "node_state", { nodeId: "n2", state: "running" }));
    apply(ev("B", "run_finished", { status: "cancelled", attached: [] }));
    assert.deepEqual([useExecutionStore.getState().resultRunId, stateOf("n2")], ["B", "running"], "开跑过的");
  } finally {
    off();
  }
});
