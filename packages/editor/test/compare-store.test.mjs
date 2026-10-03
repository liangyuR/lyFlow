// 对比的状态（store/compare，docs/compare-plan.md §3.1）：进入时顺手冻住 B、换 B 解冻、B 的节点没了自动退出。
// 纯 store 逻辑；两栏画面、冻结开关的 DOM 由 e2e compare.mjs 走真界面。
import assert from "node:assert/strict";
import { test } from "node:test";

import { matchShortcut, SHORTCUTS } from "../src/lib/keymap.ts";
import {
  COMPARE_B_GONE,
  COMPARE_NO_NODE,
  COMPARE_NO_RESULT,
  useCompareStore,
} from "../src/store/compare.ts";
import { useUiStore } from "../src/store/ui.ts";

const snap = (runId) => ({
  runId,
  preview: false,
  label: "n1",
  content: "cloud",
  outputs: [],
  cloud: null,
  cloudPort: null,
  base: null,
  maxPoints: 1000,
});

function reset({ selected = [], pinned = null, capture = null, panel = { open: false, viewerOpen: false } } = {}) {
  useCompareStore.setState({ on: false, b: null, snapshot: null, capture });
  const ui = useUiStore.getState();
  useUiStore.setState({
    toast: null,
    path: [],
    selectedNodes: new Set(selected),
    pinnedNode: pinned,
    paramPanel: { ...ui.paramPanel, ...panel },
  });
}

const state = () => {
  const s = useCompareStore.getState();
  return { on: s.on, b: s.b?.nodeId ?? null, frozen: s.snapshot?.runId ?? null, toast: useUiStore.getState().toast?.text ?? null };
};

test("预览的快捷键不与画布的撞：Ctrl+Shift+D 对比 / Ctrl+D 原地复制 / Shift+D 复制并保留输入，M 测量 / Ctrl+M 静音，Shift+Space 最大化 / Space 搜索，Alt+方向键沿连线走 / 方向键挪节点", () => {
  const key = (k, ctrlKey, shiftKey = false, altKey = false) => ({ key: k, ctrlKey, metaKey: false, shiftKey, altKey });
  const got = [key("D", true, true), key("d", true), key("D", false, true), key("m", false), key("m", true), key(" ", false, true), key(" ", false),
    key("ArrowRight", false, false, true), key("ArrowUp", false, false, true), key("ArrowRight", false), key("ArrowRight", false, true, true),
    key("1", false), key("4", false), key("1", true), key("!", false, true)]
    .map((e) => matchShortcut(e)?.id ?? null);
  assert.deepEqual(got, ["compare", "duplicate", "duplicateWired", "measure", "mute", "maximizeViewer", "search", "navDown", "navPrev", null, null,
    "viewTop", "viewIso", null, null]);
  // 表里每一个键位按下去都回到它自己：以后谁再加一个键撞上了前面的，这里先红
  const collided = SHORTCUTS.flatMap((s) => s.keys.map((combo) => {
    const parts = combo.split("+");
    const last = parts[parts.length - 1];
    const e = key(last === "Space" ? " " : last, parts.includes("Ctrl"), parts.includes("Shift") || last === "?", parts.includes("Alt"));
    return matchShortcut(e)?.id === s.id ? null : `${combo} → ${matchShortcut(e)?.id}（应为 ${s.id}）`;
  })).filter(Boolean);
  assert.deepEqual(collided, []);
});

test("预览最大化：要动画布的动作先还原它（定位、打开参数面板、面板最大化），别的不动", () => {
  const ui = () => useUiStore.getState();
  const cases = [
    // [说明, 动作, 之后还最大化着吗]
    ["定位到节点（F8、查找节点、出错的链接）", () => ui().revealNode([], "n1"), false],
    ["打开参数面板", () => ui().toggleParamPanel(true), false],
    ["参数面板最大化", () => { ui().toggleParamPanel(true); ui().setViewerMaximized(true); ui().setParamPanelMaximized(true); }, false],
    ["关参数面板", () => { ui().toggleParamPanel(true); ui().setViewerMaximized(true); ui().toggleParamPanel(false); }, true],
    ["开测量", () => ui().setViewerMeasuring(true), true],
    ["换层（连线查看器「在主 3D 视图打开」）", () => ui().setPath([]), true],
  ];
  for (const [name, act, still] of cases) {
    useUiStore.setState({ viewerMaximized: true, viewerMeasuring: false, paramPanel: { ...ui().paramPanel, open: false, maximized: false } });
    act();
    assert.equal(ui().viewerMaximized, still, name);
  }
  // 面板开着（最大化时看不见）再点「参数」：是想看面板 —— 还原，面板照旧开着（以前把看不见的面板关掉了）
  useUiStore.setState({ viewerMaximized: false, paramPanel: { ...ui().paramPanel, open: true, maximized: false } });
  ui().setViewerMaximized(true);
  ui().toggleParamPanel();
  assert.deepEqual([ui().viewerMaximized, ui().paramPanel.open], [false, true], "最大化着点「参数」");
  useUiStore.setState({ viewerMaximized: false, paramPanel: { ...ui().paramPanel, open: false, maximized: false } });
});

test("toggle：以当前节点进入，已有结果就冻住；再按一次退出", () => {
  const cases = [
    // [说明, reset 参数, 期望]
    ["没有选中节点：不进入，提示", {}, { on: false, b: null, frozen: null, toast: COMPARE_NO_NODE }],
    [
      "选中的节点已有结果：B = 它的冻结快照",
      { selected: ["n1"], capture: (slot) => snap(`run-${slot.nodeId}`) },
      { on: true, b: "n1", frozen: "run-n1", toast: null },
    ],
    [
      "还没有结果：B 跟随最新，提示",
      { selected: ["n1"], capture: () => null },
      { on: true, b: "n1", frozen: null, toast: COMPARE_NO_RESULT },
    ],
    [
      "Viewer3D 没挂着（没有 capture）：同上",
      { selected: ["n1"] },
      { on: true, b: "n1", frozen: null, toast: COMPARE_NO_RESULT },
    ],
    [
      "钉住优先于选中",
      { selected: ["n2"], pinned: "n1", capture: (slot) => snap(`run-${slot.nodeId}`) },
      { on: true, b: "n1", frozen: "run-n1", toast: null },
    ],
    ["多选不算有当前节点", { selected: ["n1", "n2"] }, { on: false, b: null, frozen: null, toast: COMPARE_NO_NODE }],
  ];
  for (const [name, setup, want] of cases) {
    reset(setup);
    useCompareStore.getState().toggle();
    assert.deepEqual(state(), want, name);
  }
  // 开着再按一次：退出，B 与快照一起丢
  reset({ selected: ["n1"], capture: () => snap("r") });
  useCompareStore.getState().toggle();
  useCompareStore.getState().toggle();
  assert.deepEqual(state(), { on: false, b: null, frozen: null, toast: null });
});

test("进入时参数面板开着、预览收起：先展开预览", () => {
  reset({ selected: ["n1"], panel: { open: true, viewerOpen: false } });
  useCompareStore.getState().toggle();
  assert.equal(useUiStore.getState().paramPanel.viewerOpen, true);
});

test("setB 换节点并解冻；没在对比时进入但不冻结；freeze / unfreeze", () => {
  const cmp = () => useCompareStore.getState();
  reset();
  cmp().freeze(snap("r0"));
  assert.equal(state().frozen, null, "没在对比时冻结无效");

  cmp().setB({ path: [], nodeId: "n1" });
  assert.deepEqual(state(), { on: true, b: "n1", frozen: null, toast: null }, "右键进入：跟随最新");

  cmp().freeze(snap("r1"));
  assert.equal(state().frozen, "r1");
  cmp().setB({ path: [], nodeId: "n2" });
  assert.deepEqual(state(), { on: true, b: "n2", frozen: null, toast: null }, "换 B = 换节点 + 解冻");

  cmp().freeze(snap("r2"));
  cmp().unfreeze();
  assert.deepEqual(state(), { on: true, b: "n2", frozen: null, toast: null }, "解冻后 B 仍是同一节点");
});

test("prune：B 的节点没了（含它所在的子图没了）就退出并提示，别的改动不管", () => {
  const seg = { nodeId: "sg", subgraphId: "s1" };
  const docWith = (topNodes, inner) => ({
    nodes: topNodes.map((id) => ({ id, op: id === "sg" ? "sub:s1" : "t.op", params: {} })),
    edges: [],
    subgraphs: inner ? { s1: { nodes: inner.map((id) => ({ id, op: "t.op", params: {} })), edges: [] } } : {},
  });
  const cases = [
    // [说明, B 槽, 图, 期望还在对比]
    ["顶层节点还在", { path: [], nodeId: "n1" }, docWith(["n1", "n2"]), true],
    ["顶层节点被删", { path: [], nodeId: "n1" }, docWith(["n2"]), false],
    ["子图里的节点还在（当前层是顶层也不退出）", { path: [seg], nodeId: "i1" }, docWith(["sg"], ["i1"]), true],
    ["子图里的节点被删", { path: [seg], nodeId: "i1" }, docWith(["sg"], ["i2"]), false],
    ["整个子图节点被删", { path: [seg], nodeId: "i1" }, docWith(["n2"], ["i1"]), false],
  ];
  for (const [name, slot, doc, alive] of cases) {
    reset();
    useCompareStore.getState().enter(slot, snap("r"));
    useCompareStore.getState().prune(doc);
    assert.deepEqual(
      [state().on, state().toast],
      alive ? [true, null] : [false, COMPARE_B_GONE],
      name,
    );
  }
});
