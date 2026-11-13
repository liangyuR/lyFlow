import assert from "node:assert/strict";
import { test } from "node:test";

import { settleModal, useModalStore } from "../src/lib/modal.ts";
import { registerViewportHandle } from "../src/lib/viewportHandle.ts";
import { useGraphStore } from "../src/store/graph.ts";
import { resetRecipes, selectRecipe, useRecipeStore } from "../src/store/recipe.ts";
import { loadRecipesFor, invalidateRecipeLoad } from "../src/store/recipeFiles.ts";
import { executionBusy, invalidateExecutionRequests, startRun, useExecutionStore } from "../src/store/execution.ts";
import { invalidatePlanRequests, requestPlan, useCacheStore } from "../src/store/cache.ts";
import { invalidateValidationRequests, requestValidate, useValidationStore } from "../src/store/validation.ts";
import { useRunHistoryStore } from "../src/store/runHistory.ts";
import { useUiStore } from "../src/store/ui.ts";
import { closeTab, findFlowByPath, flowsDirty, getFlowSnapshots, isRestoringFlow, prepareNewTab, resolveAllUnsaved, restoreFlowDocuments, startFlowTabs, switchTab, useFlowTabsStore, withFlowOperation } from "../src/store/flowTabs.ts";
import { setTransport } from "../src/transport/index.ts";

const g = () => useGraphStore.getState();
const doc = (id, name = id) => ({ schemaVersion: 1, id, name, nodes: [{ id: "n", op: "gen.synthetic", params: {} }], edges: [] });
const tick = () => new Promise((resolve) => setImmediate(resolve));
const deferred = () => { let resolve; const promise = new Promise((done) => { resolve = done; }); return { promise, resolve }; };

function fresh(tabs = [{ id: "a", doc: doc("a"), filePath: "C:/flows/A.lyflow.json", dirty: false }], activeId = tabs[0].id) {
  setTransport({ kind: "static" });
  invalidatePlanRequests();
  invalidateValidationRequests();
  invalidateRecipeLoad();
  invalidateExecutionRequests();
  useExecutionStore.getState().reset();
  useUiStore.setState({ previewing: false, path: [], autoRun: false });
  settleModal(null);
  assert.equal(restoreFlowDocuments({ tabs, activeId }), true);
  return startFlowTabs();
}

function addBlank(options) {
  const transaction = prepareNewTab(options);
  assert.ok(transaction);
  assert.equal(isRestoringFlow(), true);
  assert.equal(useRecipeStore.getState().set.recipes.length, 0, "新图迁移/布局的撤销历史不能混入旧图配方");
  g().newDoc();
  resetRecipes(null, "none");
  transaction.commit();
  assert.equal(isRestoringFlow(), false);
  return useFlowTabsStore.getState().activeId;
}

test("Flow tabs 保存图与配方的身份、撤销重做、完成结果和视口，切换不回退全局偏好", async () => {
  const stop = fresh();
  let viewport = { x: 13, y: 24, zoom: 0.8 };
  registerViewportHandle({ get: () => viewport, set: (next) => { viewport = next; } });
  try {
    resetRecipes("C:/flows/A.recipes", "ready");
    assert.equal(g().createRecipe("测试配方"), true);
    selectRecipe("测试配方", "auto");
    g().setName("A1");
    g().setName("A2");
    g().undo();
    const before = { doc: g().doc, saved: g().savedDoc, past: g().past, future: g().future, recipes: useRecipeStore.getState() };
    useUiStore.setState({ selectedNodes: new Set(["n"]), pinnedNode: "n", viewerPortPick: new Map([["n", "cloud"]]), roiFrame: { n: "roi1" } });
    const nodes = new Map([["n", { state: "done", errors: [] }]]);
    useExecutionStore.setState({ runId: "R1", resultRunId: "R1", runStatus: "ok", nodes });
    const records = [{ runId: "R1", status: "ok" }];
    useRunHistoryStore.setState({ records, seq: 1, baseline: "R1" });
    const b = addBlank();
    viewport = { x: 99, y: 88, zoom: 0.4 };
    const clipboard = { nodes: [], edges: [] };
    useUiStore.setState({ autoRun: true, clipboard, paramPanel: { open: true, maximized: false, tab: "matrix", viewerOpen: false } });
    assert.equal(await switchTab("a"), true);
    assert.equal(g().doc, before.doc);
    assert.equal(g().savedDoc, before.saved);
    assert.equal(g().past, before.past);
    assert.equal(g().future, before.future);
    assert.equal(useRecipeStore.getState().set, before.recipes.set);
    assert.equal(useRecipeStore.getState().saved, before.recipes.saved);
    assert.equal(useRecipeStore.getState().savedFiles, before.recipes.savedFiles);
    assert.equal(useRecipeStore.getState().currentId, before.recipes.currentId);
    assert.equal(useExecutionStore.getState().nodes, nodes);
    assert.equal(useExecutionStore.getState().resultRunId, "R1");
    assert.equal(useRunHistoryStore.getState().records, records);
    assert.deepEqual(viewport, { x: 13, y: 24, zoom: 0.8 });
    assert.deepEqual([...useUiStore.getState().selectedNodes], ["n"]);
    assert.equal(useUiStore.getState().roiFrame.n, "roi1");
    assert.equal(useUiStore.getState().autoRun, true);
    assert.equal(useUiStore.getState().clipboard, clipboard);
    assert.equal(useUiStore.getState().paramPanel.tab, "matrix");
    assert.equal(flowsDirty(), true);
    g().redo();
    assert.equal(g().doc.name, "A2");
    g().undo();
    assert.equal(g().doc, before.doc);
    assert.equal(await switchTab(b), true);
    assert.equal(useExecutionStore.getState().runId, null);
    assert.equal(useRecipeStore.getState().set.recipes.length, 0);
  } finally { stop(); registerViewportHandle(null); }
});

test("新建可回滚且打开可复用空tab；运行启动、配方读取、保存锁期间禁止切换", async () => {
  const stop = fresh();
  try {
    const before = g().doc;
    const transaction = prepareNewTab();
    g().newDoc();
    transaction.cancel();
    assert.equal(g().doc, before);
    assert.equal(useFlowTabsStore.getState().tabs.length, 1);
    const b = addBlank();
    const reuse = prepareNewTab({ reuseEmpty: true });
    g().loadDoc(doc("opened"), "C:/flows/Opened.lyflow.json");
    resetRecipes(null, "none");
    reuse.commit();
    assert.equal(useFlowTabsStore.getState().activeId, b);
    assert.equal(useFlowTabsStore.getState().tabs.length, 2);
    useExecutionStore.setState({ runStatus: "running" });
    assert.equal(await switchTab("a"), false);
    useExecutionStore.getState().reset();
    useRecipeStore.setState({ status: "loading" });
    assert.equal(await switchTab("a"), false);
    useRecipeStore.setState({ status: "none" });
    const gate = deferred();
    const saving = withFlowOperation(() => gate.promise);
    assert.equal(useFlowTabsStore.getState().switching, true);
    assert.equal(await switchTab("a"), false);
    await assert.rejects(startRun(g().doc, g().filePath), /正在切换或保存工作流/);
    gate.resolve("saved");
    assert.equal(await saving, "saved");
    assert.equal(useFlowTabsStore.getState().switching, false);
    assert.equal(await switchTab("a"), true);

    const started = deferred();
    setTransport({ kind: "fake", runGraph: () => started.promise });
    const running = startRun(g().doc, g().filePath);
    assert.equal(executionBusy(), true);
    assert.equal(await switchTab(b), false);
    invalidateExecutionRequests();
    started.resolve("late");
    await running;
    assert.equal(useExecutionStore.getState().runId, null);
  } finally { setTransport({ kind: "static" }); stop(); }
});

test("关闭非活动未保存tab取消或保存失败后回原tab，关闭窗口逐一检查所有dirty tab", async () => {
  const stop = fresh([
    { id: "a", doc: doc("a", "工作流A"), filePath: "C:/flows/A.lyflow.json", dirty: true },
    { id: "b", doc: doc("b", "工作流B"), filePath: "C:/flows/B.lyflow.json", dirty: true },
  ], "b");
  try {
    const closing = closeTab("a");
    assert.equal(useFlowTabsStore.getState().activeId, "a");
    settleModal("cancel");
    assert.equal(await closing, false);
    assert.equal(useFlowTabsStore.getState().activeId, "b");
    assert.equal(useFlowTabsStore.getState().tabs.length, 2);
    setTransport({ kind: "static", async saveGraph() { throw new Error("保存失败"); } });
    const failure = closeTab("a");
    settleModal("save");
    assert.equal(await failure, false);
    assert.equal(useFlowTabsStore.getState().activeId, "b");
    assert.equal(flowsDirty(), true);
    const shutdown = resolveAllUnsaved();
    assert.match(useModalStore.getState().current.message, /工作流A/);
    settleModal("discard");
    await tick();
    assert.match(useModalStore.getState().current.message, /工作流B/);
    settleModal("cancel");
    assert.equal(await shutdown, false);
    assert.equal(useFlowTabsStore.getState().activeId, "b");
    assert.equal(useFlowTabsStore.getState().tabs.length, 2);
    const discard = closeTab("a");
    settleModal("discard");
    assert.equal(await discard, true);
    assert.equal(useFlowTabsStore.getState().activeId, "b");
    assert.equal(useFlowTabsStore.getState().tabs.length, 1);
    const last = closeTab("b");
    settleModal("discard");
    assert.equal(await last, true);
    assert.equal(useFlowTabsStore.getState().tabs.length, 1);
    assert.equal(g().doc.nodes.length, 0);
    assert.equal(flowsDirty(), false);
  } finally { stop(); }
});

test("路径去重遵循Windows路径语义，备份恢复重建保存点身份及当前配方覆盖", () => {
  const recipe = { id: "recipe-a", name: "白班", values: { threshold: 7 }, graph: null, updatedAt: null };
  const set = { recipes: [recipe], defaultName: "白班" };
  const stop = fresh([
    { id: "a", doc: doc("a"), filePath: "C:/Flows/A.lyflow.json", dirty: false, recipeDirty: false,
      recipe: { set, saved: structuredClone(set), current: "白班", currentId: recipe.id, dir: "C:/Flows/A.recipes" } },
    { id: "b", doc: doc("b"), filePath: "flows/B.lyflow.json", dirty: false },
    { id: "c", doc: doc("c"), filePath: "\\\\server\\Share\\C.lyflow.json", dirty: false },
  ]);
  try {
    assert.equal(findFlowByPath("c:\\flows\\a.lyflow.json"), "a");
    assert.equal(findFlowByPath("flows/B.lyflow.json"), "b");
    assert.equal(findFlowByPath("flows/b.lyflow.json"), null);
    assert.equal(findFlowByPath("//SERVER/share/c.lyflow.json"), "c");
    assert.equal(g().doc, g().savedDoc);
    assert.equal(useRecipeStore.getState().set, useRecipeStore.getState().saved);
    assert.equal(useRecipeStore.getState().overrides, recipe.values);
    assert.equal(useRecipeStore.getState().switchedBy, "auto");
    assert.equal(flowsDirty(), false);
    g().begin();
    g().setName("拖动中的标题");
    const pending = g().pendingSnapshot;
    const history = g().past;
    const workspace = getFlowSnapshots();
    assert.equal(g().pendingSnapshot, pending, "备份只读，不提交用户正在进行的事务");
    assert.equal(g().past, history);
    g().abort();
    assert.equal(workspace.tabs.length, 3);
    assert.equal(workspace.tabs[0].snapshot.recipe.currentId, recipe.id);
  } finally { stop(); }
});

test("文档失效接口丢弃旧plan、校验和配方载入的迟到回复", async () => {
  const stop = fresh();
  const plan = deferred();
  const validation = deferred();
  const listing = deferred();
  setTransport({ kind: "tauri", planGraph: () => plan.promise, validateGraph: () => validation.promise, listRecipeDir: () => listing.promise });
  try {
    const plans = requestPlan(g().doc, g().filePath);
    const validations = requestValidate(g().doc, g().filePath);
    const recipes = loadRecipesFor(g().filePath);
    invalidatePlanRequests();
    invalidateValidationRequests();
    invalidateRecipeLoad();
    const kept = new Map([["new", { nodeId: "new" }]]);
    useCacheStore.setState({ plan: kept });
    useValidationStore.getState().set([]);
    resetRecipes(null, "none");
    plan.resolve([{ nodeId: "old" }]);
    validation.resolve([{ nodeId: "old", severity: "error", code: "late" }]);
    listing.resolve({ files: [] });
    await Promise.all([plans, validations, recipes]);
    assert.equal(useCacheStore.getState().plan, kept);
    assert.equal(useValidationStore.getState().byNode.size, 0);
    assert.equal(useRecipeStore.getState().status, "none");
  } finally { setTransport({ kind: "static" }); stop(); }
});
