// ADR-0025：迁移诊断带 edits 时，applyMigrations 在同一条撤销记录里删边、插节点、加边，
// 语义与 bridge 的 GraphDoc::apply_migration（lyflow migrate --write）一致。
import assert from 'node:assert/strict';
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
