import assert from 'node:assert/strict';
import { test } from 'node:test';

import { settleModal, useModalStore } from '../src/lib/modal.ts';
import { registerViewportHandle } from '../src/lib/viewportHandle.ts';
import { listGraphNodes, searchGraphNodes } from '../src/lib/findNodes.ts';
import { verdictTally, worstTone } from '../src/lib/outputs.ts';
import { runReadingsOf } from '../src/lib/runHistory.ts';
import { useGraphStore } from '../src/store/graph.ts';
import { resetRecipes, selectRecipe, useRecipeStore } from '../src/store/recipe.ts';
import { loadRecipesFor, invalidateRecipeLoad } from '../src/store/recipeFiles.ts';
import {
  executionBusy,
  invalidateExecutionRequests,
  judgedNodesOf,
  startRun,
  useExecutionStore,
} from '../src/store/execution.ts';
import { invalidatePlanRequests, requestPlan, useCacheStore } from '../src/store/cache.ts';
import {
  invalidateValidationRequests,
  requestValidate,
  useValidationStore,
} from '../src/store/validation.ts';
import { useRunHistoryStore } from '../src/store/runHistory.ts';
import { useUiStore } from '../src/store/ui.ts';
import {
  closeTab,
  findFlowByPath,
  flowsDirty,
  getFlowSnapshots,
  isRestoringFlow,
  prepareNewTab,
  resolveAllUnsaved,
  restoreFlowDocuments,
  startFlowTabs,
  switchTab,
  useFlowTabsStore,
  withFlowOperation,
} from '../src/store/flowTabs.ts';
import { setTransport } from '../src/transport/index.ts';

const g = () => useGraphStore.getState();
const doc = (id, name = id) => ({
  schemaVersion: 1,
  id,
  name,
  nodes: [{ id: 'n', op: 'gen.synthetic', params: {} }],
  edges: [],
});
const tick = () => new Promise((resolve) => setImmediate(resolve));
const deferred = () => {
  let resolve;
  const promise = new Promise((done) => {
    resolve = done;
  });
  return { promise, resolve };
};

function fresh(
  tabs = [{ id: 'a', doc: doc('a'), filePath: 'C:/flows/A.lyflow.json', dirty: false }],
  activeId = tabs[0].id,
) {
  setTransport({ kind: 'static' });
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
  assert.equal(
    useRecipeStore.getState().set.recipes.length,
    0,
    '新图迁移/布局的撤销历史不能混入旧图配方',
  );
  g().newDoc();
  resetRecipes(null, 'none');
  transaction.commit();
  assert.equal(isRestoringFlow(), false);
  return useFlowTabsStore.getState().activeId;
}

test('工作流会话快照：引用身份、正式判定、撤销/配方/视口与全局偏好，路径去重及备份保存基线', async () => {
  {
    const stop = fresh();
    let viewport = { x: 13, y: 24, zoom: 0.8 };
    registerViewportHandle({
      get: () => viewport,
      set: (next) => {
        viewport = next;
      },
    });
    try {
      resetRecipes('C:/flows/A.recipes', 'ready');
      assert.equal(g().createRecipe('测试配方'), true);
      selectRecipe('测试配方', 'auto');
      g().setName('A1');
      g().setName('A2');
      g().undo();
      const before = {
        doc: g().doc,
        saved: g().savedDoc,
        past: g().past,
        future: g().future,
        recipes: useRecipeStore.getState(),
      };
      useUiStore.setState({
        selectedNodes: new Set(['n']),
        pinnedNode: 'n',
        viewerPortPick: new Map([['n', 'cloud']]),
        roiFrame: { n: 'roi1' },
      });
      const nodes = new Map([['n', { state: 'done', errors: [] }]]);
      useExecutionStore.setState({ runId: 'R1', resultRunId: 'R1', runStatus: 'ok', nodes });
      const records = [{ runId: 'R1', status: 'ok' }];
      useRunHistoryStore.setState({ records, seq: 1, baseline: 'R1' });
      const b = addBlank();
      viewport = { x: 99, y: 88, zoom: 0.4 };
      const clipboard = { nodes: [], edges: [] };
      useUiStore.setState({
        autoRun: true,
        clipboard,
        paramPanel: { open: true, maximized: false, tab: 'matrix', viewerOpen: false },
      });
      assert.equal(await switchTab('a'), true);
      for (const [name, actual, expected] of [
        ['文档', g().doc, before.doc],
        ['保存点', g().savedDoc, before.saved],
        ['撤销栈', g().past, before.past],
        ['重做栈', g().future, before.future],
        ['配方集合', useRecipeStore.getState().set, before.recipes.set],
        ['配方保存点', useRecipeStore.getState().saved, before.recipes.saved],
        ['配方文件映射', useRecipeStore.getState().savedFiles, before.recipes.savedFiles],
        ['当前配方', useRecipeStore.getState().currentId, before.recipes.currentId],
        ['完成节点表', useExecutionStore.getState().nodes, nodes],
        ['结果运行ID', useExecutionStore.getState().resultRunId, 'R1'],
        ['调参记录', useRunHistoryStore.getState().records, records],
        ['全局剪贴板', useUiStore.getState().clipboard, clipboard],
      ])
        assert.equal(actual, expected, `切回工作流保持${name}身份`);
      assert.deepEqual(
        {
          viewport,
          selection: [...useUiStore.getState().selectedNodes],
          roi: useUiStore.getState().roiFrame.n,
          autoRun: useUiStore.getState().autoRun,
          panel: useUiStore.getState().paramPanel.tab,
        },
        {
          viewport: { x: 13, y: 24, zoom: 0.8 },
          selection: ['n'],
          roi: 'roi1',
          autoRun: true,
          panel: 'matrix',
        },
        '文档视角/选中/ROI独立，全局偏好不随文档回退',
      );
      assert.equal(flowsDirty(), true);
      g().redo();
      assert.equal(g().doc.name, 'A2');
      g().undo();
      assert.equal(g().doc, before.doc);
      assert.equal(await switchTab(b), true);
      assert.equal(useExecutionStore.getState().runId, null);
      assert.equal(useRecipeStore.getState().set.recipes.length, 0);
    } finally {
      stop();
      registerViewportHandle(null);
    }
  }
  {
    const a = doc('a');
    a.nodes.push({ id: 'm', op: 'gen.synthetic', params: {} });
    const stop = fresh([
      { id: 'a', doc: a, filePath: 'C:/flows/A.lyflow.json', dirty: false },
      { id: 'b', doc: doc('b'), filePath: 'C:/flows/B.lyflow.json', dirty: false },
    ]);
    const finish = (runId, verdicts, preview = false) => {
      const execution = useExecutionStore.getState();
      execution.beginRun(runId, [], preview);
      let seq = 0;
      const emit = (event) =>
        execution.apply({ schemaVersion: 1, runId, at: '', seq: seq++, ...event });
      emit({
        kind: 'run_started',
        mode: preview ? 'preview' : 'full',
        plan: Object.keys(verdicts),
      });
      for (const [nodeId, verdict] of Object.entries(verdicts)) {
        emit({
          kind: 'node_state',
          nodeId,
          state: 'done',
          stats: {
            outputs: [
              {
                port: 'value',
                type: 'Measurement',
                value: { kind: 'Measurement', value: 1, unit: 'mm', verdict },
              },
            ],
          },
        });
      }
      emit({ kind: 'run_finished', status: 'ok', durationMs: 1 });
      return useExecutionStore.getState().nodes;
    };
    const seen = [];
    const off = useExecutionStore.subscribe((state) => seen.push(judgedNodesOf(state)));
    try {
      finish('formal-a', { n: 'high', m: 'margin' });
      const formalA = useExecutionStore.getState().nodes;
      off();
      const previewA = finish('preview-a', { n: 'ok', m: 'ok' }, true);
      assert.equal(
        judgedNodesOf(useExecutionStore.getState()),
        formalA,
        '预览即使没有渲染订阅也保留正式结果',
      );
      const observe = useExecutionStore.subscribe((state) => seen.push(judgedNodesOf(state)));
      try {
        assert.equal(await switchTab('b'), true);
        assert.equal(
          judgedNodesOf(useExecutionStore.getState()).size,
          0,
          '从文档恢复的新tab没有旧判定',
        );
        const formalB = finish('formal-b', { n: 'ok' });
        assert.equal(judgedNodesOf(useExecutionStore.getState()), formalB);
        assert.equal(await switchTab('a'), true);
        assert.equal(useExecutionStore.getState().nodes, previewA);
        assert.equal(useExecutionStore.getState().preview, true);
        assert.equal(seen.at(-1), formalA, 'store订阅在恢复预览快照时立即收到该tab的正式判定');
        const restored = judgedNodesOf(useExecutionStore.getState());
        assert.equal(restored, formalA);
        assert.deepEqual(verdictTally(runReadingsOf(restored)), {
          ng: 1,
          margin: 1,
          ok: 0,
          unmeasured: 0,
        });
        const entries = listGraphNodes(g().doc, new Map());
        const ids = (query) =>
          searchGraphNodes(entries, query, undefined, (id) =>
            worstTone(restored.get(id)?.stats?.outputs),
          ).map((hit) => hit.entry.id);
        assert.deepEqual(ids('is:ng'), ['n']);
        assert.deepEqual(ids('is:margin'), ['m']);
        useExecutionStore.getState().apply({
          schemaVersion: 1,
          runId: 'formal-b',
          at: '',
          seq: 50,
          kind: 'log',
          level: 'info',
          message: 'late',
        });
        assert.equal(
          judgedNodesOf(useExecutionStore.getState()),
          formalA,
          '其他tab迟到事件不覆盖判定',
        );
        const blank = addBlank();
        assert.equal(judgedNodesOf(useExecutionStore.getState()).size, 0, '新建tab清空正式判定');
        assert.equal(await switchTab('b'), true);
        assert.equal(judgedNodesOf(useExecutionStore.getState()), formalB, 'B保留自己的正式判定');
        assert.equal(await switchTab('a'), true);
        assert.equal(judgedNodesOf(useExecutionStore.getState()), formalA);
        assert.equal(
          getFlowSnapshots().tabs.find((tab) => tab.id === 'a').snapshot.judgedNodes,
          formalA,
        );
        assert.equal(await switchTab(blank), true);
        assert.equal(judgedNodesOf(useExecutionStore.getState()).size, 0);
        useExecutionStore.getState().reset();
        assert.equal(judgedNodesOf(useExecutionStore.getState()).size, 0);
      } finally {
        observe();
      }
    } finally {
      off();
      stop();
    }
  }
  {
    const recipe = {
      id: 'recipe-a',
      name: '白班',
      values: { threshold: 7 },
      graph: null,
      updatedAt: null,
    };
    const set = { recipes: [recipe], defaultName: '白班' };
    const stop = fresh([
      {
        id: 'a',
        doc: doc('a'),
        filePath: 'C:/Flows/A.lyflow.json',
        dirty: false,
        recipeDirty: false,
        recipe: {
          set,
          saved: structuredClone(set),
          current: '白班',
          currentId: recipe.id,
          dir: 'C:/Flows/A.recipes',
        },
      },
      { id: 'b', doc: doc('b'), filePath: 'flows/B.lyflow.json', dirty: false },
      { id: 'c', doc: doc('c'), filePath: '\\\\server\\Share\\C.lyflow.json', dirty: false },
    ]);
    try {
      for (const [path, expected] of [
        ['c:\\flows\\a.lyflow.json', 'a'],
        ['flows/B.lyflow.json', 'b'],
        ['flows/b.lyflow.json', null],
        ['//SERVER/share/c.lyflow.json', 'c'],
      ])
        assert.equal(findFlowByPath(path), expected, `路径去重：${path}`);
      assert.equal(g().doc, g().savedDoc);
      assert.equal(useRecipeStore.getState().set, useRecipeStore.getState().saved);
      assert.equal(useRecipeStore.getState().overrides, recipe.values);
      assert.equal(useRecipeStore.getState().switchedBy, 'auto');
      assert.equal(flowsDirty(), false);
      g().begin();
      g().setName('拖动中的标题');
      const pending = g().pendingSnapshot;
      const history = g().past;
      const workspace = getFlowSnapshots();
      assert.equal(g().pendingSnapshot, pending, '备份只读，不提交用户正在进行的事务');
      assert.equal(g().past, history);
      g().abort();
      assert.equal(workspace.tabs.length, 3);
      assert.equal(workspace.tabs[0].snapshot.recipe.currentId, recipe.id);
    } finally {
      stop();
    }
  }
  {
    const original = doc('a');
    original.nodes.push({ id: 'm', op: 'gen.synthetic', params: {} });
    const stop = fresh([
      { id: 'a', doc: original, filePath: 'C:/flows/A.lyflow.json', dirty: false },
      { id: 'b', doc: doc('b'), filePath: 'C:/flows/B.lyflow.json', dirty: false },
    ]);
    try {
      g().deleteNodes(['n']);
      g().deleteNodes(['m']);
      g().undo();
      assert.ok(g().past.length > 0 && g().future.length > 0);
      const before = g().doc;
      assert.equal(await switchTab('b'), true);
      g().setTopologyLocked(true);
      assert.equal(await switchTab('a'), true);
      assert.deepEqual(
        [g().topologyLocked, g().canUndo(), g().canRedo()],
        [true, false, false],
        '后台文档锁前的结构撤销与重做不能在切回后绕过全局拓扑锁',
      );
      g().undo();
      g().redo();
      assert.equal(g().doc, before);
      assert.deepEqual(g().deleteNodes(['m']), []);
      assert.equal(g().lastRejection, '拓扑已锁定');

      g().begin();
      g().moveNodes([{ id: 'm', position: { x: 12, y: 34 } }]);
      g().commit();
      const moved = g().doc;
      const past = g().past;
      assert.equal(await switchTab('b'), true);
      assert.equal(await switchTab('a'), true);
      assert.equal(g().topologyLocked, true);
      assert.equal(g().past, past, '锁定后合法编辑的撤销记录仍独立保留');
      g().undo();
      assert.equal(g().doc, before);
      g().redo();
      assert.equal(g().doc, moved);
    } finally {
      g().setTopologyLocked(false);
      stop();
    }
  }
});

test('工作流事务：新建回滚/空tab复用、运行/保存/配方锁与失效后迟到回复隔离', async () => {
  {
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
      g().loadDoc(doc('opened'), 'C:/flows/Opened.lyflow.json');
      resetRecipes(null, 'none');
      reuse.commit();
      assert.equal(useFlowTabsStore.getState().activeId, b);
      assert.equal(useFlowTabsStore.getState().tabs.length, 2);
      useExecutionStore.setState({ runStatus: 'running' });
      assert.equal(await switchTab('a'), false);
      useExecutionStore.getState().reset();
      useRecipeStore.setState({ status: 'loading' });
      assert.equal(await switchTab('a'), false);
      useRecipeStore.setState({ status: 'none' });
      const gate = deferred();
      const saving = withFlowOperation(() => gate.promise);
      assert.equal(useFlowTabsStore.getState().switching, true);
      assert.equal(await switchTab('a'), false);
      await assert.rejects(startRun(g().doc, g().filePath), /正在切换或保存工作流/);
      gate.resolve('saved');
      assert.equal(await saving, 'saved');
      assert.equal(useFlowTabsStore.getState().switching, false);
      assert.equal(await switchTab('a'), true);

      const started = deferred();
      setTransport({ kind: 'fake', runGraph: () => started.promise });
      const running = startRun(g().doc, g().filePath);
      assert.equal(executionBusy(), true);
      assert.equal(await switchTab(b), false);
      invalidateExecutionRequests();
      started.resolve('late');
      await running;
      assert.equal(useExecutionStore.getState().runId, null);
    } finally {
      setTransport({ kind: 'static' });
      stop();
    }
  }
  {
    const stop = fresh();
    const plan = deferred();
    const validation = deferred();
    const listing = deferred();
    setTransport({
      kind: 'tauri',
      planGraph: () => plan.promise,
      validateGraph: () => validation.promise,
      listRecipeDir: () => listing.promise,
    });
    try {
      const plans = requestPlan(g().doc, g().filePath);
      const validations = requestValidate(g().doc, g().filePath);
      const recipes = loadRecipesFor(g().filePath);
      invalidatePlanRequests();
      invalidateValidationRequests();
      invalidateRecipeLoad();
      const kept = new Map([['new', { nodeId: 'new' }]]);
      useCacheStore.setState({ plan: kept });
      useValidationStore.getState().set([]);
      resetRecipes(null, 'none');
      plan.resolve([{ nodeId: 'old' }]);
      validation.resolve([{ nodeId: 'old', severity: 'error', code: 'late' }]);
      listing.resolve({ files: [] });
      await Promise.all([plans, validations, recipes]);
      assert.equal(useCacheStore.getState().plan, kept);
      assert.equal(useValidationStore.getState().byNode.size, 0);
      assert.equal(useRecipeStore.getState().status, 'none');
    } finally {
      setTransport({ kind: 'static' });
      stop();
    }
  }
});

test('关闭非活动未保存tab取消或保存失败后回原tab，关闭窗口逐一检查所有dirty tab', async () => {
  const stop = fresh(
    [
      { id: 'a', doc: doc('a', '工作流A'), filePath: 'C:/flows/A.lyflow.json', dirty: true },
      { id: 'b', doc: doc('b', '工作流B'), filePath: 'C:/flows/B.lyflow.json', dirty: true },
    ],
    'b',
  );
  try {
    const closing = closeTab('a');
    assert.equal(useFlowTabsStore.getState().activeId, 'a');
    settleModal('cancel');
    assert.equal(await closing, false);
    assert.equal(useFlowTabsStore.getState().activeId, 'b');
    assert.equal(useFlowTabsStore.getState().tabs.length, 2);
    setTransport({
      kind: 'static',
      async saveGraph() {
        throw new Error('保存失败');
      },
    });
    const failure = closeTab('a');
    settleModal('save');
    assert.equal(await failure, false);
    assert.equal(useFlowTabsStore.getState().activeId, 'b');
    assert.equal(flowsDirty(), true);
    const shutdown = resolveAllUnsaved();
    assert.match(useModalStore.getState().current.message, /工作流A/);
    settleModal('discard');
    await tick();
    assert.match(useModalStore.getState().current.message, /工作流B/);
    settleModal('cancel');
    assert.equal(await shutdown, false);
    assert.equal(useFlowTabsStore.getState().activeId, 'b');
    assert.equal(useFlowTabsStore.getState().tabs.length, 2);
    const discard = closeTab('a');
    settleModal('discard');
    assert.equal(await discard, true);
    assert.equal(useFlowTabsStore.getState().activeId, 'b');
    assert.equal(useFlowTabsStore.getState().tabs.length, 1);
    const last = closeTab('b');
    settleModal('discard');
    assert.equal(await last, true);
    assert.equal(useFlowTabsStore.getState().tabs.length, 1);
    assert.equal(g().doc.nodes.length, 0);
    assert.equal(flowsDirty(), false);
  } finally {
    stop();
  }
});
