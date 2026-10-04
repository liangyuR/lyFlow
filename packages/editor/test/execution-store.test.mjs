// 执行 store 的「节点表反映哪一次运行」（store/execution 的 resultRunId，ADR-0027）：带 targets 的运行可能在桥接层排队，
// 开跑（run_started）之前视图照旧按上一次取；排队的那次没开跑就收场时，被抢占的那次留下的半截状态落成「已取消」。
// 真排队、真抢占由 bridge/src/execution.rs 的单测与 e2e noderun.mjs 的抢占组走；这里只钉 store 怎么接事件。
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";

import { matchShortcut } from "../src/lib/keymap.ts";
import { docNodeKey, listGraphNodes, parseFinderQuery, searchGraphNodes } from "../src/lib/findNodes.ts";
import { culpritOf, errorNodeIds, failedUpstream, revealError, revealNodeError } from "../src/lib/revealError.ts";
import { describeEventNode, locateEventNode } from "../src/lib/subgraph.ts";
import { useGraphStore } from "../src/store/graph.ts";
import { useManifestStore } from "../src/store/manifest.ts";
import {
  aggregatedNodes,
  cancelCurrentRun,
  onNodeTransition,
  restartRun,
  runControlsOf,
  startRun,
  useExecutionStore,
} from "../src/store/execution.ts";
import { useUiStore } from "../src/store/ui.ts";
import { setTransport } from "../src/transport/index.ts";

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

test("判定看的节点表：预览、节点表还不是这一次的、松手自动补的那一次在跑 —— 都留着上一次正式运行的", async () => {
  const { judgedNodesOf } = await import("../src/store/execution.ts");
  const formal = new Map([["j", { state: "done", errors: [] }]]);
  const later = new Map();
  const base = { nodes: formal, preview: false, runId: "r1", resultRunId: "r1", runStatus: "ok", request: null };
  assert.equal(judgedNodesOf(base), formal, "正式运行跑完：就是它");
  const cases = [
    ["预览", { ...base, nodes: later, preview: true }],
    ["带 targets 的运行还没开跑（节点表还是上一次的）", { ...base, nodes: later, runId: "r2" }],
    ["松手自动补的那一次在跑", { ...base, nodes: later, runId: "r2", resultRunId: "r2", runStatus: "running", request: { auto: true } }],
  ];
  for (const [name, s] of cases) assert.equal(judgedNodesOf(s), formal, name);
  assert.equal(judgedNodesOf({ ...base, nodes: later, runId: "r3", resultRunId: "r3", runStatus: "running", request: {} }), later,
    "手按的运行在跑：看这一次的");
});

test("查找节点（Ctrl+F）：整张图连子图里面的一起列，按名字模糊找；库算子里面进不去，不列；is:muted / is:error / op: 筛选", () => {
  const all = listGraphNodes(sgDoc, sgOps);
  assert.deepEqual(
    all.map((e) => [e.id, e.title, e.parents.join(" › ")]),
    [
      ["top", "体素降采样", ""],
      ["a", "预处理", ""],
      ["a/v", "体素降采样", "预处理"],
      ["a/n", "去噪", "预处理"],
      ["a/n/x", "离群点", "预处理 › 去噪"],
      ["lib1", "库去噪", ""],
    ],
  );
  assert.deepEqual(all[4].path, [{ nodeId: "a", subgraphId: "s1" }, { nodeId: "n", subgraphId: "s2" }]);
  const ids = (q) => searchGraphNodes(all, q).map((h) => h.entry.id);
  assert.deepEqual(ids("离群"), ["a/n/x"], "起过标题的按标题找");
  assert.equal(searchGraphNodes(all, "统计").find((h) => h.entry.id === "a/n/x")?.fieldIndex, 2, "也认算子名");
  assert.deepEqual(ids("体素"), ["top", "a/v"], "同分保持文档顺序");
  assert.equal(ids("").length, all.length, "空查询列全部");
  assert.deepEqual(ids("zzz"), []);

  // 筛选词：静音的（顶层的 top、子图里的 a/v）、这次出错的、按算子 id / 算子名的一段；认不得的 is:xxx 当普通字
  const s1 = sgDoc.subgraphs.s1;
  const mutedDoc = {
    ...sgDoc,
    nodes: sgDoc.nodes.map((n) => (n.id === "top" ? { ...n, bypass: true } : n)),
    subgraphs: { ...sgDoc.subgraphs, s1: { ...s1, nodes: s1.nodes.map((n) => (n.id === "v" ? { ...n, bypass: true } : n)) } },
  };
  const muted = listGraphNodes(mutedDoc, sgOps);
  const failed = (id) => id === "a/n/x";
  const tones = { "a/v": "ng", top: "margin", "a/n/x": "ok" };
  const filtered = [
    // [查询, 期望]
    ["is:muted", ["top", "a/v"]],
    ["is:静音 体素", ["top", "a/v"]],
    ["is：muted 预处理", ["a/v"]],
    ["is:error", ["a/n/x"]],
    ["op:voxel", ["top", "a/v"]],
    ["op:统计", ["a/n/x"]],
    ["op:sub:s2", ["a/n"]],
    ["op:voxel is:muted op:grid", ["top", "a/v"]],
    ["is:foo", []],
    ["is:constructor", []],
    ["is:__proto__ 体素", []],
    ["is:ng", ["a/v"]],
    ["is:边界", ["top"]],
    ["is:不合格 is:muted", ["a/v"]],
  ];
  for (const [q, want] of filtered) {
    assert.deepEqual(searchGraphNodes(muted, q, failed, (id) => tones[id] ?? null).map((h) => h.entry.id), want, q);
  }
  assert.deepEqual(searchGraphNodes(muted, "is:error").map((h) => h.entry.id), [], "不给出错判断就当都没出错");
  assert.deepEqual(parseFinderQuery("  op:Voxel  is:muted  离群 "), { text: "离群", muted: true, error: false, ng: false, margin: false, ops: ["voxel"] });
  // 子图定义是共用的：两个实例里列出来的 v 是文档里同一个节点（状态栏只数一次，Alt+Enter 不说它「在别的层没选」）
  const twice = listGraphNodes({ ...mutedDoc, nodes: [...mutedDoc.nodes, { id: "a2", op: "sub:s1" }] }, sgOps);
  const vs = twice.filter((e) => e.localId === "v");
  assert.deepEqual([vs.map((e) => e.id), new Set(vs.map(docNodeKey)).size], [["a/v", "a2/v"], 1]);
  assert.notEqual(docNodeKey(twice.find((e) => e.id === "top")), docNodeKey(vs[0]), "顶层的节点与子图里的不混");

  const key = (k) => ({ key: k, ctrlKey: true, metaKey: false, shiftKey: false, altKey: false });
  assert.equal(matchShortcut(key("f"))?.id, "findNode");
});

test("聚合出来的子图节点记着错误来自哪个内部节点；只有一个内部节点的子图也是", () => {
  const err = { phase: "execute", code: "bad_param", message: "Leaf size 太小", paramPath: "leafSize" };
  const nodes = new Map([
    ["top", { state: "done", errors: [] }],
    ["a/v", { state: "done", errors: [] }],
    ["a/n/x", { state: "error", errors: [err] }],
    ["b/only", { state: "error", errors: [err] }],
    ["c/p", { state: "error", errors: [err] }],
    ["c/q", { state: "error", errors: [err, err] }],
  ]);
  const top = aggregatedNodes([], nodes);
  assert.equal(top.get("top"), nodes.get("top"), "叶子节点原样复用");
  assert.deepEqual([top.get("a").state, top.get("a").errorSource, top.get("a").children], ["error", "a/n/x", { total: 2, finished: 1 }]);
  assert.deepEqual([top.get("b").errorSource, top.get("b").children], ["b/only", { total: 1, finished: 0 }]);
  // 每一条错误都记着来源（检查器里逐条写明）：与 errors 一一对应
  assert.deepEqual(top.get("a").errorSources, ["a/n/x"]);
  assert.deepEqual([top.get("c").errors.length, top.get("c").errorSources], [3, ["c/p", "c/q", "c/q"]]);
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
  // 预览区空态的「定位到出错的地方」：子图节点打开到里面出错的那个
  assert.equal(revealNodeError([], "a"), true);
  assert.deepEqual(at(), ["a", "v", "leafSize"], "子图节点的错误：打开到里面那个");

  // 被连带没执行的节点是因为谁：沿入边往上找最近的出错节点（l → m → n，旁支 j → k → n）
  const chain = { edges: [
    { from: { node: "l", port: "o" }, to: { node: "m", port: "i" } },
    { from: { node: "m", port: "o" }, to: { node: "n", port: "i" } },
    { from: { node: "k", port: "o" }, to: { node: "n", port: "j" } },
    { from: { node: "j", port: "o" }, to: { node: "k", port: "i" } },
  ] };
  for (const [states, want, why] of [
    [{ l: "error", m: "cancelled" }, "l", "隔一层的根因"],
    [{ l: "error", m: "error" }, "m", "两个都错：取最近的"],
    [{ l: "done", m: "cancelled", k: "error" }, "k", "旁支上的"],
    [{ l: "done", m: "cancelled" }, null, "这一层里找不到（根因在外面）"],
    // m 跑完了（flow.fallback 接住了 l 的错）：l 不是 n 没执行的原因，真正的在旁支上
    [{ l: "error", m: "done", k: "cancelled", j: "error" }, "j", "不穿过跑完了的节点"],
  ]) {
    assert.equal(failedUpstream(chain, "n", (id) => states[id]), want, why);
  }

  useExecutionStore.setState({ nodes: new Map() });
  assert.equal(revealError(1), false, "没有出错的节点：什么都不做");

  const key = (k, shiftKey) => ({ key: k, ctrlKey: false, metaKey: false, shiftKey, altKey: false });
  assert.deepEqual([matchShortcut(key("F8", false))?.id, matchShortcut(key("F8", true))?.id], ["nextError", "prevError"]);
});

test("连带没执行的根因在子图外面：从子图节点那里往外一层接着找，不跳到整张图的第一个错", () => {
  // 顶层 z（不相干的错，节点表里排第一）、p → S；S 里面 x 接的是 S 的入口（不是边），被 p 连带取消
  const doc = {
    schemaVersion: 1,
    id: "d",
    nodes: [{ id: "z", op: "t" }, { id: "p", op: "t" }, { id: "S", op: "sub:s9" }],
    edges: [{ id: "e1", from: { node: "p", port: "o" }, to: { node: "S", port: "in" } }],
    subgraphs: {
      s9: { name: "S", nodes: [{ id: "x", op: "t" }], edges: [], params: [], outputs: [],
            inputs: [{ name: "in", type: "PointCloud", to: [{ node: "x", port: "cloud" }] }] },
    },
  };
  const upstream = { phase: "execute", code: "upstream_failed", message: "上游节点 p 失败，未执行" };
  const nodes = new Map([
    ["z", { state: "error", errors: [{ phase: "execute", code: "bad_param", message: "坏了" }] }],
    ["p", { state: "error", errors: [{ phase: "execute", code: "bad_param", message: "坏了" }] }],
    ["S/x", { state: "cancelled", errors: [upstream] }],
  ]);
  const inner = [{ nodeId: "S", subgraphId: "s9" }];
  const c = culpritOf(doc, inner, "x", nodes);
  assert.deepEqual([c?.path.length, c?.id], [0, "p"]);
  assert.equal(culpritOf(doc, [], "z", new Map([["z", { state: "cancelled", errors: [upstream] }]])), null, "哪一层都没有");
});

test("工具栏的运行 / 重跑 / 取消中：人发起的运行在跑时是重跑；预览、自动补的那次不变；取消发出去之后是取消中", () => {
  const base = { runStatus: "running", preview: false, cancelling: false, request: {} };
  const cases = [
    ["没在跑", { ...base, runStatus: "idle" }, ["run", "off"]],
    ["人发起的运行", base, ["rerun", "on"]],
    ["预览运行（拖参数）：按钮不闪", { ...base, preview: true }, ["run", "on"]],
    ["松手后补的那一次", { ...base, request: { targets: ["n"], auto: true } }, ["run", "on"]],
    ["取消发出去了", { ...base, cancelling: true }, ["rerun", "cancelling"]],
    ...["ok", "error", "cancelled"].map((st) => [`收场：${st}`, { ...base, runStatus: st }, ["run", "off"]]),
  ];
  for (const [name, s, want] of cases) {
    const c = runControlsOf(s);
    assert.deepEqual([c.run, c.cancel], want, name);
  }
});

test("取消中的起落；重跑照原来的范围再来一次，目标删掉了就跑整张图", async () => {
  const calls = [];
  const cancels = [];
  let failCancel = false;
  setTransport({
    kind: "fake",
    async runGraph(_doc, _path, opts) {
      calls.push(opts);
      return `R${calls.length}`;
    },
    async cancelRun(id) {
      cancels.push(id);
      if (failCancel) throw new Error("断了");
    },
  });
  useExecutionStore.getState().reset();
  seqs = new Map();
  useGraphStore.setState({ doc: sgDoc });
  useManifestStore.setState({ operatorsById: sgOps });

  await startRun(sgDoc, null, { targets: ["a/n/x"], force: ["a/n/x"] });
  await cancelCurrentRun();
  await cancelCurrentRun();
  let s = useExecutionStore.getState();
  assert.deepEqual([cancels, s.cancelling, s.runStatus], [["R1"], true, "running"], "再按一次 Esc 不再发一遍");

  await restartRun(sgDoc, null);
  s = useExecutionStore.getState();
  assert.deepEqual([calls[1].targets, calls[1].force, calls[1].mode, s.runId, s.cancelling],
    [["a/n/x"], ["a/n/x"], "full", "R2", false], "重跑：同样的范围；取消中到此为止");
  assert.equal("auto" in calls[1], false, "auto 不交给 core");
  apply(ev("R2", "run_finished", { status: "ok" }));
  assert.deepEqual([useExecutionStore.getState().runStatus, useExecutionStore.getState().cancelling], ["ok", false]);

  await startRun(sgDoc, null, {});
  failCancel = true;
  await assert.rejects(cancelCurrentRun());
  assert.equal(useExecutionStore.getState().cancelling, false, "取消没发出去：撤回「取消中」");
  failCancel = false;

  await startRun(sgDoc, null, { targets: ["a/gone"] });
  await restartRun(sgDoc, null);
  assert.equal(calls.at(-1).targets, undefined, "要跑到的节点删掉了：退回整张图");

  // 范围被忘掉（换了一张图时编辑器清掉它）：还在跑也只是运行整张图
  await startRun(sgDoc, null, { targets: ["a/n/x"] });
  useExecutionStore.setState({ request: null });
  assert.equal(runControlsOf(useExecutionStore.getState()).run, "rerun", "按钮照旧是重跑");
  await restartRun(sgDoc, null);
  assert.equal(calls.at(-1).targets, undefined, "没有范围可沿用：整张图");
});

test("自动运行的调度（lib/preview）：提交了一步攒 250 ms 补；没跑过不补、跑着时排着的照补；拖动松手并进那一次；换图、关掉撤掉；整图运行撤掉、带 targets 的并进来", async () => {
  const { autoRunOnCommit, endPreview, skipAutoRunFor } = await import("../src/lib/preview.ts");
  const { onCommitted } = await import("../src/store/graph.ts");
  const manifest = JSON.parse(readFileSync(new URL("../../../schema/examples/manifest.example.json", import.meta.url), "utf8"));
  useManifestStore.getState().replaceBundle(manifest, 1);
  const calls = [];
  setTransport({
    kind: "tauri",
    async runGraph(_doc, _path, opts) {
      calls.push(opts.targets ?? null);
      return `auto${calls.length}`;
    },
  });
  const voxel = (id) => ({ id, op: "filter.voxel_grid", params: {} });
  const base = { schemaVersion: 1, id: "d", nodes: [voxel("n1"), voxel("n2")],
    edges: [{ id: "e", from: { node: "n1", port: "cloud" }, to: { node: "n2", port: "cloud" } }] };
  const g = () => useGraphStore.getState();
  const ran = (states, status = "ok") => useExecutionStore.setState({ runStatus: status, runId: "r0",
    nodes: new Map(Object.entries(states).map(([id, state]) => [id, { state, errors: [] }])) });
  const settle = () => new Promise((resolve) => setTimeout(resolve, 330));
  const leaf = (v) => [v, v, v];
  useUiStore.setState({ autoRun: true, previewing: false, path: [], selectedNodes: new Set(), pinnedNode: null });
  g().loadDoc(structuredClone(base), null);
  const stop = onCommitted(autoRunOnCommit);
  try {
    useExecutionStore.getState().reset();
    g().setParam("n1", "leafSize", leaf(0.02));
    await settle();
    assert.deepEqual(calls.splice(0), [], "这张图还没跑过：不补");

    ran({ n1: "done", n2: "done" });
    g().setParam("n1", "leafSize", leaf(0.03));
    g().setParam("n1", "leafSize", leaf(0.031));
    await settle();
    assert.deepEqual(calls.splice(0), [["n1"]], "跑过了：连着两下只补一次，算改的那个");

    ran({ n1: "done", n2: "idle" }, "running");
    g().setParam("n2", "leafSize", leaf(0.04));
    await settle();
    assert.deepEqual(calls.splice(0), [["n2"]], "正在跑、排着的节点开跑时回到 idle：照补");

    ran({ n1: "done", n2: "done" });
    useUiStore.setState({ previewing: true });
    g().begin();
    g().setParam("n1", "leafSize", leaf(0.05));
    g().commit();
    assert.deepEqual(calls.splice(0), [], "拖动松手的 commit 只攒");
    endPreview("n1");
    await settle();
    assert.deepEqual(calls.splice(0), [["n1"]], "endPreview 并进那一次，之后不再补");

    ran({ n1: "done", n2: "done" });
    g().setParam("n1", "leafSize", leaf(0.06));
    g().loadDoc(structuredClone(base), null);
    ran({ n1: "done", n2: "done" });
    await settle();
    assert.deepEqual(calls.splice(0), [], "换了一张图：攒着的不跑到新图上（节点 id 同名）");

    g().setParam("n1", "leafSize", leaf(0.07));
    await startRun(g().doc, null, { targets: ["n2"] });
    await settle();
    assert.deepEqual(calls.splice(0), [["n2", "n1"]], "人点的「运行到 n2」：攒着的 n1 并进来，之后不再补");

    ran({ n1: "done", n2: "done" });
    g().setParam("n1", "leafSize", leaf(0.08));
    await startRun(g().doc, null, { auto: true });
    await settle();
    assert.deepEqual(calls.splice(0), [null], "整图运行（切配方补的那种）：已经包含，撤掉攒着的");

    ran({ n1: "done", n2: "done" });
    g().setParam("n1", "leafSize", leaf(0.085));
    skipAutoRunFor("n1");
    await settle();
    assert.deepEqual(calls.splice(0), [], "2D 框还没画齐（RoiLayer 叫它撤）：这一下攒的撤掉");

    ran({ n1: "done", n2: "done" });
    g().setParam("n1", "leafSize", leaf(0.09));
    useUiStore.setState({ autoRun: false });
    await settle();
    assert.deepEqual(calls.splice(0), [], "关掉自动运行：攒着的也不发");
  } finally {
    stop();
    useUiStore.setState({ autoRun: true, previewing: false });
    useExecutionStore.getState().reset();
  }
});
