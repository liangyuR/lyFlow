// 执行 store 的「节点表反映哪一次运行」（store/execution 的 resultRunId，ADR-0027）：带 targets 的运行可能在桥接层排队，
// 开跑（run_started）之前视图照旧按上一次取；排队的那次没开跑就收场时，被抢占的那次留下的半截状态落成「已取消」。
// 真排队、真抢占由 bridge/src/execution.rs 的单测与 e2e noderun.mjs 的抢占组走；这里只钉 store 怎么接事件。
import assert from "node:assert/strict";
import { test } from "node:test";

import { matchShortcut } from "../src/lib/keymap.ts";
import { errorNodeIds, revealError } from "../src/lib/revealError.ts";
import { describeEventNode, locateEventNode } from "../src/lib/subgraph.ts";
import { useGraphStore } from "../src/store/graph.ts";
import { useManifestStore } from "../src/store/manifest.ts";
import { aggregatedNodes, onNodeTransition, useExecutionStore } from "../src/store/execution.ts";
import { useUiStore } from "../src/store/ui.ts";

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

// 子图内部节点的错误（事件 id 是路径，ADR-0010）：顶层原来只看得到「这个子图红了」，要自己进去找
const sgDoc = {
  schemaVersion: 1,
  id: "doc",
  nodes: [
    { id: "top", op: "filter.voxel_grid" },
    { id: "a", op: "sub:s1", ui: { title: "预处理" } },
    { id: "lib1", op: "lib.denoise" },
  ],
  edges: [],
  subgraphs: {
    s1: { name: "S1", nodes: [{ id: "v", op: "filter.voxel_grid" }, { id: "n", op: "sub:s2" }], edges: [], inputs: [], outputs: [], params: [] },
    s2: { name: "去噪", nodes: [{ id: "x", op: "filter.statistical_outlier", ui: { title: "离群点" } }], edges: [], inputs: [], outputs: [], params: [] },
  },
};
const sgOps = new Map([
  ["filter.voxel_grid", { label: "体素降采样" }],
  ["filter.statistical_outlier", { label: "统计离群点剔除" }],
  ["lib.denoise", { label: "库去噪" }],
]);

test("事件 id → 打开到哪一层、选中谁、一层层叫什么", () => {
  const cases = [
    // [事件 id, 路径（子图节点 id）, 选中, 名字, 落到的就是它本身]
    ["top", [], "top", ["体素降采样"], true],
    ["a/v", ["a"], "v", ["预处理", "体素降采样"], true],
    ["a/n/x", ["a", "n"], "x", ["预处理", "去噪", "离群点"], true],
    // 库算子的内部进不去：停在库算子身上，参数红框不标
    ["lib1/inner", [], "lib1", ["库去噪"], false],
  ];
  for (const [id, path, local, names, exact] of cases) {
    const d = describeEventNode(sgDoc, sgOps, id);
    assert.deepEqual(
      [d.reveal?.path.map((seg) => seg.nodeId), d.reveal?.localId, d.names, d.reveal?.exact],
      [path, local, names, exact],
      id,
    );
  }
  assert.equal(locateEventNode(sgDoc, "gone/x"), null, "节点被删了");
  assert.deepEqual(locateEventNode(sgDoc, "a/n/x").path, [{ nodeId: "a", subgraphId: "s1" }, { nodeId: "n", subgraphId: "s2" }]);
});

test("聚合出来的子图节点记着错误来自哪个内部节点；只有一个内部节点的子图也是", () => {
  const err = { phase: "execute", code: "bad_param", message: "Leaf size 太小", paramPath: "leafSize" };
  const nodes = new Map([
    ["top", { state: "done", errors: [] }],
    ["a/v", { state: "done", errors: [] }],
    ["a/n/x", { state: "error", errors: [err] }],
    ["b/only", { state: "error", errors: [err] }],
  ]);
  const top = aggregatedNodes([], nodes);
  assert.equal(top.get("top"), nodes.get("top"), "叶子节点原样复用");
  assert.deepEqual([top.get("a").state, top.get("a").errorSource, top.get("a").children], ["error", "a/n/x", { total: 2, finished: 1 }]);
  assert.deepEqual([top.get("b").errorSource, top.get("b").children], ["b/only", { total: 1, finished: 0 }]);
  // 进到 a 这一层：n 是它的子图节点，来源还是那个完整 id
  assert.equal(aggregatedNodes([{ nodeId: "a", subgraphId: "s1" }], nodes).get("n").errorSource, "a/n/x");
});

test("revealNode：打开到那一层、选中、请画布移过去；同一个节点再点一次也要动", () => {
  useUiStore.setState({ path: [], selectedNodes: new Set(), focusedDiagnostic: null, revealRequest: null });
  const path = [{ nodeId: "a", subgraphId: "s1" }];
  useUiStore.getState().revealNode(path, "v", "leafSize");
  let s = useUiStore.getState();
  assert.deepEqual([s.path, [...s.selectedNodes], s.focusedDiagnostic, s.revealRequest?.nodeId],
    [path, ["v"], { nodeId: "v", paramPath: "leafSize" }, "v"]);
  const seq = s.revealRequest.seq;
  const samePath = s.path;
  useUiStore.getState().revealNode(path, "v");
  s = useUiStore.getState();
  assert.equal(s.revealRequest.seq, seq + 1);
  assert.equal(s.path, samePath, "同一层不换路径对象");
});

test("在出错的节点之间跳：第一个是根因，F8 往后、Shift+F8 往前，到头绕回；上游失败连带的不算", () => {
  const err = (paramPath) => ({ phase: "execute", code: "bad_param", message: "坏了", paramPath });
  useGraphStore.setState({ doc: sgDoc });
  useManifestStore.setState({ operatorsById: sgOps });
  useExecutionStore.setState({
    nodes: new Map([
      ["a/v", { state: "error", errors: [err("leafSize")] }],
      ["a/n/x", { state: "cancelled", errors: [{ phase: "execute", code: "upstream_failed", message: "上游失败" }] }],
      ["top", { state: "error", errors: [err("leafSize")] }],
    ]),
  });
  useUiStore.setState({ path: [], selectedNodes: new Set(), focusedDiagnostic: null, revealRequest: null });
  assert.deepEqual(errorNodeIds(), ["a/v", "top"]);

  const at = () => {
    const s = useUiStore.getState();
    return [s.path.map((p) => p.nodeId).join("/"), [...s.selectedNodes][0], s.focusedDiagnostic?.paramPath];
  };
  assert.equal(revealError(0), true);
  assert.deepEqual(at(), ["a", "v", "leafSize"], "第一个：打开到子图里");
  revealError(1);
  assert.deepEqual(at(), ["", "top", "leafSize"], "往后：回到顶层的那个");
  revealError(1);
  assert.deepEqual(at(), ["a", "v", "leafSize"], "到头绕回");
  revealError(-1);
  assert.deepEqual(at(), ["", "top", "leafSize"], "往前");

  useExecutionStore.setState({ nodes: new Map() });
  assert.equal(revealError(1), false, "没有出错的节点：什么都不做");

  const key = (k, shiftKey) => ({ key: k, ctrlKey: false, metaKey: false, shiftKey, altKey: false });
  assert.deepEqual([matchShortcut(key("F8", false))?.id, matchShortcut(key("F8", true))?.id], ["nextError", "prevError"]);
});
