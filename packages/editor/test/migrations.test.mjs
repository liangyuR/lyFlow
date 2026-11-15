// ADR-0025：迁移诊断带 edits 时，applyMigrations 在同一条撤销记录里删边、插节点、加边，
// 语义与 bridge 的 GraphDoc::apply_migration（lyflow migrate --write）一致。
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';

import { useGraphStore } from '../src/store/graph.ts';
import { useManifestStore } from '../src/store/manifest.ts';
import { useUiStore } from '../src/store/ui.ts';
import { usePythonStore } from '../src/store/python.ts';
import {
  defaultPythonConfig,
  normalizePythonConfig,
  pythonConfigProblem,
  unsupportedPythonSettings,
} from '../src/lib/pythonConfig.ts';
import {
  setTransport,
  HttpTransport,
  TauriTransport,
  StaticTransport,
} from '../src/transport/index.ts';

function doc() {
  return {
    schemaVersion: 1,
    id: '01J8XQZ4K7N3M2R5V8W1YB6TCF',
    nodes: [
      { id: 'a', op: 't.src', ui: { position: { x: 0, y: 0 } } },
      { id: 'b', op: 't.src', ui: { position: { x: 0, y: 200 } } },
      { id: 's', op: 't.sink', opVersion: '1.1.0', ui: { position: { x: 500, y: 100 } } },
    ],
    edges: [
      { id: 'e1', from: { node: 'a', port: 'out' }, to: { node: 's', port: 'old1' } },
      { id: 'e2', from: { node: 'b', port: 'out' }, to: { node: 's', port: 'old2' } },
      { id: 'm_s_in', from: { node: 'a', port: 'out' }, to: { node: 'b', port: 'unrelated' } },
    ],
  };
}

const action = {
  kind: 'migration',
  nodeId: 's',
  severity: 'warning',
  phase: 'validate',
  code: 'migration',
  message: '节点已从 t.sink v1.1.0 迁移到 t.sink v2.0.0',
  op: 't.sink',
  opVersion: '2.0.0',
  params: {},
  notes: ['old1、old2 收进新插的 t.pack，接到 in'],
  edits: {
    removeEdges: [
      { id: 'e1', from: { node: 'a', port: 'out' }, to: { node: 's', port: 'old1' } },
      { id: 'e2', from: { node: 'b', port: 'out' }, to: { node: 's', port: 'old2' } },
    ],
    addNodes: [
      { id: 's_pack', op: 't.pack', opVersion: '1.0.0', params: {}, title: '打包', near: 's' },
    ],
    addEdges: [
      { id: 'm_s_pack_x', from: { node: 'a', port: 'out' }, to: { node: 's_pack', port: 'x' } },
      { id: 'm_s_pack_y', from: { node: 'b', port: 'out' }, to: { node: 's_pack', port: 'y' } },
      { id: 'm_s_in', from: { node: 's_pack', port: 'out' }, to: { node: 's', port: 'in' } },
    ],
  },
};

test('版本化契约升级夹具在编辑器迁移、保存及撤销后保留来源证据', () => {
  const before = JSON.parse(
    readFileSync(
      new URL(
        '../../../core/tests/fixtures/contract-upgrade/plane-calib.lyflow.json',
        import.meta.url,
      ),
      'utf8',
    ),
  );
  const rule = before.contractUpgrade.rules[0];
  const old = before.edges[0];
  const adapter = { node: rule.adapterId, port: 'calib' };
  const change = {
    ...action,
    nodeId: 'consumer',
    op: 'util.reroute',
    opVersion: '1.0.0',
    params: {},
    edits: {
      removeEdges: [old],
      addNodes: [
        {
          id: rule.adapterId,
          op: 'image.upgrade_plane_calib',
          opVersion: '1.0.0',
          params: { missingUnit: 'mm', imageFrame: 'camera', planeFrame: 'table' },
        },
      ],
      addEdges: [
        { id: 'contract_explicit_calib_in', from: old.from, to: adapter },
        { ...old, from: adapter },
      ],
    },
  };
  const historyEntry = {
    schemaVersion: 1,
    request: structuredClone(before.contractUpgrade),
    appliedSources: [structuredClone(rule.source)],
  };
  change.graphUpdate = {
    contractUpgradeRetirement: {
      request: structuredClone(before.contractUpgrade),
      historyEntry,
      requirements: [
        {
          source: rule.source,
          target: rule.target,
          adapter: change.edits.addNodes[0],
          port: 'calib',
        },
      ],
    },
  };
  const g = useGraphStore.getState();
  g.setTopologyLocked(false);
  g.loadDoc(structuredClone(before), 'upgrade.lyflow.json');
  assert.equal(g.applyMigrations([change]), 1);
  const saved = JSON.parse(JSON.stringify(useGraphStore.getState().doc));
  assert.equal(saved.contractUpgrade, undefined);
  assert.deepEqual(saved.meta.contractUpgradeHistory, [historyEntry]);
  assert.equal(saved.nodes.find((n) => n.id === 'load').opVersion, '1.0.0');
  assert.equal(saved.nodes.find((n) => n.id === rule.adapterId).params.missingUnit, 'mm');
  assert.equal(saved.edges.length, 2);
  useGraphStore.getState().undo();
  assert.deepEqual(JSON.parse(JSON.stringify(useGraphStore.getState().doc)), before);

  const multi = structuredClone(before);
  multi.meta = { owner: 'keep', contractUpgradeHistory: [{ schemaVersion: 2, future: true }] };
  multi.nodes.push({ id: 'other', op: 'util.reroute', opVersion: '1.0.0', params: {} });
  multi.edges.push({ id: 'other-edge', from: old.from, to: { node: 'other', port: 'in' } });
  const secondRule = {
    ...structuredClone(rule),
    target: { node: 'other', port: 'in' },
    adapterId: 'other_calib',
  };
  multi.contractUpgrade.rules.push(secondRule);
  const second = structuredClone(change);
  second.nodeId = 'other';
  second.edits.removeEdges = [multi.edges[1]];
  second.edits.addNodes[0].id = 'other_calib';
  second.edits.addEdges = [
    { id: 'contract_other_calib_in', from: old.from, to: { node: 'other_calib', port: 'calib' } },
    { ...multi.edges[1], from: { node: 'other_calib', port: 'calib' } },
  ];
  const multiHistory = {
    schemaVersion: 1,
    request: structuredClone(multi.contractUpgrade),
    appliedSources: multi.contractUpgrade.rules.map((r) => structuredClone(r.source)),
  };
  const retirement = {
    request: structuredClone(multi.contractUpgrade),
    historyEntry: multiHistory,
    requirements: [
      {
        source: rule.source,
        target: rule.target,
        adapter: change.edits.addNodes[0],
        port: 'calib',
      },
      {
        source: secondRule.source,
        target: secondRule.target,
        adapter: second.edits.addNodes[0],
        port: 'calib',
      },
    ],
  };
  const first = structuredClone(change);
  delete first.graphUpdate;
  second.graphUpdate = { contractUpgradeRetirement: retirement };
  g.loadDoc(structuredClone(multi), 'partial.lyflow.json');
  g.setTopologyLocked(true);
  assert.equal(g.applyMigrations([first, second]), 0);
  assert.deepEqual(useGraphStore.getState().doc.contractUpgrade, multi.contractUpgrade);
  g.setTopologyLocked(false);
  assert.equal(g.applyMigrations([second]), 1);
  const partial = JSON.parse(JSON.stringify(useGraphStore.getState().doc));
  assert.deepEqual(partial.contractUpgrade, multi.contractUpgrade);
  assert.deepEqual(partial.meta, multi.meta);
  g.loadDoc(partial, 'partial.lyflow.json');
  first.graphUpdate = { contractUpgradeRetirement: retirement };
  assert.equal(g.applyMigrations([first]), 1);
  const completed = JSON.parse(JSON.stringify(useGraphStore.getState().doc));
  assert.equal(completed.contractUpgrade, undefined);
  assert.equal(completed.meta.owner, 'keep');
  assert.deepEqual(completed.meta.contractUpgradeHistory, [
    ...multi.meta.contractUpgradeHistory,
    multiHistory,
  ]);
  assert.deepEqual(completed.edges.map((e) => e.id).sort(), [
    'calib-edge',
    'contract_explicit_calib_in',
    'contract_other_calib_in',
    'other-edge',
  ]);
  const historicalActive = structuredClone(completed);
  historicalActive.contractUpgrade = structuredClone(multi.contractUpgrade);
  const graphOnly = { ...first, nodeId: '', op: '', opVersion: '', params: {}, edits: undefined };
  for (const bypass of [true, null, 'false', 0]) {
    const disabled = structuredClone(historicalActive);
    disabled.meta = { owner: 'keep', contractUpgradeHistory: [] };
    disabled.nodes.find((node) => node.id === rule.adapterId).bypass = bypass;
    g.loadDoc(disabled, 'disabled-adapter.lyflow.json');
    assert.equal(g.applyMigrations([graphOnly]), 0);
    assert.deepEqual(JSON.parse(JSON.stringify(useGraphStore.getState().doc)), disabled);
    const enabled = structuredClone(disabled);
    enabled.nodes.find((node) => node.id === rule.adapterId).bypass = false;
    g.loadDoc(enabled, 'enabled-adapter.lyflow.json');
    assert.equal(g.applyMigrations([graphOnly]), 1);
    assert.equal(useGraphStore.getState().doc.contractUpgrade, undefined);
    assert.deepEqual(useGraphStore.getState().doc.meta.contractUpgradeHistory, [multiHistory]);
  }
  g.loadDoc(historicalActive, 'historical-active.lyflow.json');
  assert.equal(g.applyMigrations([graphOnly]), 1);
  assert.equal(useGraphStore.getState().doc.contractUpgrade, undefined);
  assert.equal(useGraphStore.getState().doc.meta.contractUpgradeHistory.length, 2);
  useUiStore.getState().setPath([]);
  g.deleteNodes(['consumer']);
  assert.equal(useGraphStore.getState().doc.contractUpgrade, undefined);
  const remaining = useGraphStore.getState().doc.nodes.map((n) => n.id);
  assert.ok(g.composeSubgraph(remaining));
  assert.equal(useGraphStore.getState().doc.contractUpgrade, undefined);
  assert.deepEqual(
    useGraphStore.getState().doc.meta.contractUpgradeHistory,
    completed.meta.contractUpgradeHistory,
  );
  for (const sourceOp of ['image.load_calib', 'image.renamed_calib']) {
    g.loadDoc(structuredClone(before), 'mixed-hooks.lyflow.json');
    const sourceMigration = {
      ...action,
      nodeId: 'load',
      op: sourceOp,
      opVersion: '1.1.0',
      params: {},
      edits: undefined,
    };
    const targetMigration = {
      ...action,
      nodeId: 'consumer',
      op: 'test.consumer_v2',
      opVersion: '2.0.0',
      params: {},
      edits: {
        removeEdges: [change.edits.addEdges[1]],
        addNodes: [],
        addEdges: [{ ...change.edits.addEdges[1], to: { node: 'consumer', port: 'newIn' } }],
      },
    };
    assert.equal(g.applyMigrations([sourceMigration]), 0);
    assert.deepEqual(JSON.parse(JSON.stringify(useGraphStore.getState().doc)), before);
    g.setTopologyLocked(true);
    assert.equal(g.applyMigrations([change, sourceMigration]), 0);
    assert.deepEqual(JSON.parse(JSON.stringify(useGraphStore.getState().doc)), before);
    g.setTopologyLocked(false);
    assert.equal(g.applyMigrations([change, sourceMigration, targetMigration]), 2);
    const mixed = JSON.parse(JSON.stringify(useGraphStore.getState().doc));
    assert.equal(mixed.contractUpgrade, undefined);
    assert.deepEqual(mixed.meta.contractUpgradeHistory, [historyEntry]);
    assert.equal(mixed.nodes.find((n) => n.id === 'load').op, sourceOp);
    assert.equal(mixed.nodes.find((n) => n.id === 'load').opVersion, '1.1.0');
    assert.equal(mixed.edges.find((e) => e.id === old.id).to.port, 'newIn');
  }
});

test('edits：删两条旧边、插一个节点、加三条边，一次撤销全部回去；不带 edits 的只改版本与参数', () => {
  const g = useGraphStore.getState();
  const before = doc();
  g.loadDoc(structuredClone(before), 'g.lyflow.json');
  assert.equal(g.applyMigrations([action]), 1);

  const after = JSON.parse(JSON.stringify(useGraphStore.getState().doc));
  assert.equal(after.nodes.find((n) => n.id === 's').opVersion, '2.0.0');
  const pack = after.nodes.find((n) => n.id === 's_pack');
  assert.deepEqual(pack, {
    id: 's_pack',
    op: 't.pack',
    opVersion: '1.0.0',
    params: {},
    ui: { position: { x: 280, y: 240 }, title: '打包' },
  });
  const wires = after.edges.map(
    (e) => `${e.id}:${e.from.node}.${e.from.port}>${e.to.node}.${e.to.port}`,
  );
  // m_s_in 已被一条无关的边占了，新边加后缀
  assert.deepEqual(wires, [
    'm_s_in:a.out>b.unrelated',
    'm_s_pack_x:a.out>s_pack.x',
    'm_s_pack_y:b.out>s_pack.y',
    'm_s_in_2:s_pack.out>s.in',
  ]);
  assert.equal(useGraphStore.getState().dirty, true);

  useGraphStore.getState().undo();
  assert.deepEqual(JSON.parse(JSON.stringify(useGraphStore.getState().doc)), before);

  // 不带 edits 的迁移照旧只改版本与参数，节点和边都不动
  const { edits: _, ...plain } = action;
  assert.equal(useGraphStore.getState().applyMigrations([{ ...plain, params: { level: 2 } }]), 1);
  const migrated = JSON.parse(JSON.stringify(useGraphStore.getState().doc));
  const s = migrated.nodes.find((n) => n.id === 's');
  assert.equal(s.opVersion, '2.0.0');
  assert.deepEqual(s.params, { level: 2 });
  assert.deepEqual(
    migrated.nodes.map((n) => n.id),
    ['a', 'b', 's'],
  );
  assert.deepEqual(migrated.edges, before.edges);

  // 拓扑锁着：改连线的那条整条不写回（节点停在老版本），只改版本与参数的照写
  useGraphStore.getState().loadDoc(structuredClone(before), 'g.lyflow.json');
  useGraphStore.getState().setTopologyLocked(true);
  const paramOnly = { ...plain, nodeId: 'a', op: 't.src', opVersion: '2.0.0', params: { k: 1 } };
  assert.equal(useGraphStore.getState().applyMigrations([action, paramOnly]), 1);
  const locked = JSON.parse(JSON.stringify(useGraphStore.getState().doc));
  assert.equal(useGraphStore.getState().lastRejection, '拓扑已锁定');
  assert.equal(locked.nodes.find((n) => n.id === 's').opVersion, '1.1.0');
  assert.equal(locked.nodes.find((n) => n.id === 'a').opVersion, '2.0.0');
  assert.deepEqual(locked.edges, before.edges);
  useGraphStore.getState().setTopologyLocked(false);
});

test('Python 节点管理：可选/只读宿主、异步刷新与缺依赖保护、迟到隔离及传输契约', async () => {
  const originalFetch = globalThis.fetch;
  const originalWindow = globalThis.window;
  const deferred = () => {
    let resolve;
    const promise = new Promise((done) => {
      resolve = done;
    });
    return { promise, resolve };
  };
  const config = {
    executable: 'C:/Python/python.exe',
    packagePaths: ['C:/nodes/package.py'],
    timeoutMs: 60_000,
  };
  const status = {
    ...config,
    configured: true,
    ready: true,
    operatorCount: 1,
    pythonVersion: '3.12.0',
    numpyVersion: '2.1.0',
    problems: [],
  };
  const settings = { supported: true, editable: true, config, status };
  const native = {
    id: 'native.keep',
    label: '原生算子',
    version: '1.0.0',
    category: 'Test',
    inputs: [],
    outputs: [{ name: 'cloud', type: 'PointCloud' }],
    params: [],
  };
  const python = {
    ...native,
    id: 'py.pkg.node',
    inputs: [{ name: 'cloud', type: 'PointCloud' }],
    outputs: [{ name: 'value', type: 'Record' }],
    params: [{ name: 'gain', type: 'float', default: 2 }],
  };
  const bundle = {
    schemaVersion: 1,
    types: ['PointCloud', 'Image', 'Measurement', 'Record'].map((name) => ({
      name,
      color: '#123456',
    })),
    operators: [native, python],
  };
  const refresh = { status, manifest: bundle };
  const state = () => usePythonStore.getState();
  try {
    assert.deepEqual(defaultPythonConfig(), {
      executable: '',
      packagePaths: [],
      timeoutMs: 60_000,
    });
    assert.deepEqual(
      normalizePythonConfig({
        executable: ' "C:/Python/python.exe" ',
        packagePaths: [' "C:/nodes/package.py" ', '', 'C:/nodes/package.py'],
        timeoutMs: 60_000,
      }),
      config,
      '路径清理并去重，超时保持毫秒',
    );
    for (const [name, candidate, valid] of [
      ['缺解释器', { ...config, executable: '' }, false],
      ['负超时', { ...config, timeoutMs: -1 }, false],
      ['超过一天', { ...config, timeoutMs: 86_400_001 }, false],
      ['最小毫秒边界', { ...config, timeoutMs: 1 }, true],
      ['一天边界', { ...config, timeoutMs: 86_400_000 }, true],
      ['小数毫秒', { ...config, timeoutMs: 1.5 }, false],
      ['无效数字', { ...config, timeoutMs: Number.NaN }, false],
      ['有效设置', config, true],
    ])
      assert.equal(pythonConfigProblem(candidate) === null, valid, name);
    setTransport({ kind: 'static' });
    await state().load();
    assert.equal(state().settings.supported, false, '旧宿主没有可选方法时仍能继续');
    assert.deepEqual(await new StaticTransport().getPythonSettings(), unsupportedPythonSettings());
    let writes = 0;
    setTransport({
      kind: 'http',
      getPythonSettings: async () => ({ ...settings, editable: false }),
      setPythonSettings: async () => {
        writes++;
        return refresh;
      },
    });
    await state().load();
    state().edit({ executable: 'changed' });
    assert.deepEqual(
      [state().draft, await state().save(), await state().refresh(), writes],
      [config, false, false, 0],
      '只读宿主不改草稿且不写入',
    );
    const gate = deferred();
    const received = [];
    let fail = false;
    setTransport({
      kind: 'tauri',
      getPythonSettings: async () => settings,
      setPythonSettings: async (next) => {
        received.push(next);
        return gate.promise;
      },
      refreshPythonNodes: async () => {
        if (fail) throw new Error('Python 服务不可用');
        return refresh;
      },
      planGraph: async () => [],
      validateGraph: async () => [],
    });
    await state().load();
    useManifestStore
      .getState()
      .replaceBundle(
        { ...bundle, operators: [...bundle.operators, { ...python, id: 'py.removed' }] },
        0,
      );
    useGraphStore.getState().loadDoc(
      {
        schemaVersion: 1,
        id: 'python-test',
        nodes: [
          { id: 'n', op: native.id },
          { id: 'p', op: python.id, params: { gain: 4 } },
        ],
        edges: [{ id: 'np', from: { node: 'n', port: 'cloud' }, to: { node: 'p', port: 'cloud' } }],
      },
      null,
    );
    const graph = useGraphStore.getState().doc;
    useUiStore.setState({ inspectedOperator: 'py.removed', selectedNodes: new Set(['p']) });
    state().edit({
      executable: ' "C:/Python/python.exe" ',
      packagePaths: ['C:/nodes/package.py', 'C:/nodes/package.py'],
    });
    const saving = state().save();
    assert.equal(state().busy, true, '等待后端期间保持忙态');
    state().edit({ executable: 'ignored' });
    assert.equal(await state().refresh(), false, '忙态不重复刷新');
    const problems = [
      { path: config.executable, message: 'NumPy 未安装', traceback: 'ImportError: numpy' },
    ];
    gate.resolve({
      status: { ...status, ready: false, operatorCount: 0, problems },
      manifest: { ...bundle, operators: [native] },
    });
    assert.equal(await saving, true);
    assert.deepEqual(received, [config]);
    assert.deepEqual(
      {
        ready: state().settings.status.ready,
        problems: state().settings.status.problems,
        inspected: useUiStore.getState().inspectedOperator,
        selection: [...useUiStore.getState().selectedNodes],
        operators: [...useManifestStore.getState().operatorsById.keys()],
      },
      { ready: false, problems, inspected: null, selection: ['p'], operators: ['native.keep'] },
      '缺依赖不影响原生算子，不丢未知节点选择',
    );
    assert.equal(useGraphStore.getState().doc, graph, '刷新保留节点参数与连线的文档身份');
    assert.equal(await state().refresh(), true);
    assert.deepEqual(
      useManifestStore.getState().operatorsById.get(python.id).params,
      python.params,
      '现有表单直接消费Python参数描述',
    );
    const kept = useManifestStore.getState().bundle;
    fail = true;
    assert.deepEqual(
      [await state().refresh(), state().error, state().busy],
      [false, 'Python 服务不可用', false],
    );
    assert.equal(useManifestStore.getState().bundle, kept, '失败保留有效manifest');
    const late = deferred();
    setTransport({ kind: 'http', getPythonSettings: () => late.promise });
    const pending = state().load();
    setTransport({ kind: 'static' });
    await state().load();
    late.resolve(settings);
    await pending;
    assert.equal(state().settings.supported, false, '旧宿主迟到回复不能覆盖新宿主');
    const calls = [];
    globalThis.fetch = async (url, init = {}) => {
      calls.push([url, init.method ?? 'GET', init.body ? JSON.parse(init.body) : null]);
      return new Response(JSON.stringify(init.method ? refresh : settings));
    };
    const http = new HttpTransport('http://localhost:9000');
    assert.deepEqual(
      [
        await http.getPythonSettings(),
        await http.setPythonSettings(config),
        await http.refreshPythonNodes(),
      ],
      [settings, refresh, refresh],
    );
    assert.deepEqual(
      calls,
      [
        ['http://localhost:9000/lyflow/python/settings', 'GET', null],
        ['http://localhost:9000/lyflow/python/settings', 'PUT', config],
        ['http://localhost:9000/lyflow/python/refresh', 'POST', null],
      ],
      'HTTP路径、动词和正文',
    );
    calls.length = 0;
    globalThis.window = {
      __TAURI_INTERNALS__: {
        invoke: async (command, args) => {
          calls.push([command, args]);
          return command === 'get_python_settings' ? settings : refresh;
        },
      },
    };
    const tauri = new TauriTransport();
    assert.deepEqual(
      [
        await tauri.getPythonSettings(),
        await tauri.setPythonSettings(config),
        await tauri.refreshPythonNodes(),
      ],
      [settings, refresh, refresh],
    );
    assert.deepEqual(
      calls,
      [
        ['get_python_settings', {}],
        ['set_python_settings', { config }],
        ['refresh_python_nodes', {}],
      ],
      'Tauri命令与config参数',
    );
  } finally {
    globalThis.fetch = originalFetch;
    if (originalWindow === undefined) delete globalThis.window;
    else globalThis.window = originalWindow;
    setTransport({ kind: 'static', planGraph: async () => [], validateGraph: async () => [] });
    useUiStore.setState({ inspectedOperator: null, selectedNodes: new Set() });
    useGraphStore.getState().setTopologyLocked(false);
  }
});
