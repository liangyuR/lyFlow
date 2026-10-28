import assert from 'node:assert/strict';
import { fork, spawnSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import test from 'node:test';
import { snapshotStore } from './snapshots.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const cli = process.env.LYFLOW_CLI ?? path.resolve(here, '../../../bridge/target/debug/lyflow.exe');
const fixture = () => fs.mkdtempSync(path.join(os.tmpdir(), 'lyflow-snapshot-test-'));
const doc = (file) => ({
  schemaVersion: 1,
  id: 'snapshot-test',
  nodes: [{ id: 'load', op: 'io.load_pcd', params: { path: file } }],
  edges: [],
});
function writePcd(file, x) {
  fs.writeFileSync(
    file,
    `VERSION 0.7\nFIELDS x y z\nSIZE 4 4 4\nTYPE F F F\nCOUNT 1 1 1\nWIDTH 1\nHEIGHT 1\nPOINTS 1\nDATA ascii\n${x} 2 3\n`,
  );
}
function run(argv) {
  const r = spawnSync(cli, argv, { encoding: 'utf8' });
  assert.equal(r.status, 0, r.stderr);
  return r.stdout.trim().split(/\r?\n/).map(JSON.parse);
}
async function server(root, work) {
  const child = fork(
    path.join(here, 'server.mjs'),
    ['--root', root, '--cli', cli, '--port', '0', '--work-dir', work],
    { stdio: ['ignore', 'pipe', 'pipe', 'ipc'] },
  );
  let stderr = '';
  child.stderr.on('data', (c) => (stderr += c));
  const port = await new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      child.kill();
      reject(new Error('server startup timeout: ' + stderr));
    }, 10000);
    child.once('message', (m) => {
      clearTimeout(timer);
      resolve(m.port);
    });
    child.once('exit', (code) => {
      clearTimeout(timer);
      reject(new Error(`server exit ${code}: ${stderr}`));
    });
  });
  return {
    url: `http://127.0.0.1:${port}`,
    stop: () =>
      new Promise((resolve) => {
        child.once('exit', resolve);
        child.kill();
      }),
  };
}

test('same run snapshot survives input mutation and HTTP restart without rerunning', async () => {
  const root = fixture();
  const input = path.join(root, 'input.pcd');
  writePcd(input, 1);
  const work = path.join(root, 'work');
  let app = await server(root, work);
  try {
    const response = await fetch(app.url + '/lyflow/run', {
      method: 'POST',
      body: JSON.stringify({ doc: doc(input), capture: { policy: 'all' } }),
    });
    assert.equal(response.status, 200, await response.clone().text());
    const { runId, snapshotId } = await response.json();
    assert.equal(runId, snapshotId);
    const manifest = await (await fetch(`${app.url}/lyflow/snapshots/${snapshotId}`)).json();
    assert.equal(manifest.runId, runId);
    const info = run(['info']).at(-1);
    assert.ok(Object.hasOwn(manifest.engine, 'buildIdentity'));
    assert.equal(manifest.engine.buildIdentity, info.buildIdentity);
    assert.equal(manifest.engine.corePath, info.corePath);
    assert.equal(manifest.outputs[0].nodeId, 'load');
    assert.equal(manifest.inputs.length, 1);
    assert.equal(manifest.inputs[0].unchangedDuringRun, true);
    const getCloud = async () => {
      const res = await fetch(`${app.url}/lyflow/snapshots/${snapshotId}/clouds/load/cloud`);
      assert.equal(res.status, 200, await res.clone().text());
      return Buffer.from(await res.arrayBuffer());
    };
    const before = await getCloud();
    assert.equal(before.readFloatLE(40), 1);
    writePcd(input, 99);
    assert.deepEqual(await getCloud(), before);
    await app.stop();
    app = await server(root, work);
    assert.deepEqual(await getCloud(), before);
    const summaryResponse = await fetch(`${app.url}/lyflow/runs/${runId}/summary`);
    assert.equal(summaryResponse.status, 200);
    assert.deepEqual(await summaryResponse.json(), manifest.summary);
    const savedResponse = await fetch(`${app.url}/lyflow/runs/${runId}/snapshot`);
    assert.equal(savedResponse.status, 200);
    assert.equal((await savedResponse.json()).snapshotId, snapshotId);
    const unavailable = await fetch(
      `${app.url}/lyflow/snapshots/${snapshotId}/clouds/load/missing`,
    );
    assert.equal(unavailable.status, 410);
  } finally {
    await app.stop();
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test('CLI quota and selection report missing payloads; immutable directory cannot be overwritten', () => {
  const root = fixture();
  try {
    const input = path.join(root, 'in.pcd');
    writePcd(input, 3);
    const graph = path.join(root, 'graph.json');
    fs.writeFileSync(graph, JSON.stringify(doc(input)));
    const output = path.join(root, 'snapshot');
    const events = run(['run', graph, '--snapshot-dir', output, '--capture-max-bytes', '0']);
    const manifest = JSON.parse(fs.readFileSync(path.join(output, 'snapshot.json')));
    assert.equal(manifest.runId, events.find((e) => e.kind === 'run_started').runId);
    assert.equal(manifest.outputs[0].missing, 'quota_exceeded');
    assert.equal(manifest.capture.capturedBytes, 0);
    assert.notEqual(spawnSync(cli, ['run', graph, '--snapshot-dir', output]).status, 0);
    const selected = path.join(root, 'selected');
    run([
      'run',
      graph,
      '--snapshot-dir',
      selected,
      '--capture-policy',
      'selected',
      '--capture-output',
      'load:missing',
    ]);
    const m = JSON.parse(fs.readFileSync(path.join(selected, 'snapshot.json')));
    assert.equal(m.outputs.find((o) => o.port === 'cloud').missing, 'not_selected');
    assert.equal(m.outputs.find((o) => o.port === 'missing').missing, 'output_unavailable');
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test('eval rows refer to their own execution snapshots', () => {
  const root = fixture();
  try {
    const input = path.join(root, 'in.pcd');
    writePcd(input, 4);
    const graph = path.join(root, 'graph.json');
    fs.writeFileSync(graph, JSON.stringify(doc(input)));
    const snapshots = path.join(root, 'snapshots');
    const lines = run([
      'eval',
      graph,
      '--metric',
      'nodes.load.cloud.elementCount',
      '--snapshot-root',
      snapshots,
    ]);
    const row = lines.find((e) => e.kind === 'eval_row');
    assert.ok(row.snapshotId);
    const manifest = JSON.parse(
      fs.readFileSync(path.join(snapshots, row.snapshotId, 'snapshot.json')),
    );
    assert.equal(manifest.snapshotId, row.snapshotId);
    assert.equal(manifest.outputs[0].elementCount, row.metrics['nodes.load.cloud.elementCount']);
    assert.ok(fs.existsSync(path.join(snapshots, row.snapshotId, manifest.outputs[0].file)));
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test('image payload is captured as native LYIM from the run', () => {
  const root = fixture();
  try {
    const image = path.join(root, 'input.pgm');
    fs.writeFileSync(
      image,
      Buffer.concat([Buffer.from('P5\n2 2\n255\n'), Buffer.from([0, 64, 128, 255])]),
    );
    const graph = path.join(root, 'graph.json');
    fs.writeFileSync(
      graph,
      JSON.stringify({
        schemaVersion: 1,
        id: 'image',
        nodes: [{ id: 'image', op: 'io.load_image', params: { path: image } }],
        edges: [],
      }),
    );
    const output = path.join(root, 'image-snapshot');
    run(['run', graph, '--snapshot-dir', output]);
    const manifest = JSON.parse(fs.readFileSync(path.join(output, 'snapshot.json')));
    const item = manifest.outputs.find((o) => o.type === 'Image');
    assert.ok(item.file);
    const bytes = fs.readFileSync(path.join(output, item.file));
    assert.equal(bytes.subarray(0, 4).toString(), 'LYIM');
    assert.equal(bytes.readUInt32LE(4), 2);
    assert.equal(bytes.readUInt32LE(8), 2);
    assert.deepEqual([...bytes.subarray(48, 52)], [0, 64, 128, 255]);
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test('total quota refuses a new snapshot and keeps existing references', () => {
  const root = fixture();
  try {
    const input = path.join(root, 'input.pcd');
    writePcd(input, 3);
    const graph = path.join(root, 'graph.json');
    fs.writeFileSync(graph, JSON.stringify(doc(input)));
    const first = path.join(root, 'first');
    run(['run', graph, '--snapshot-dir', first]);
    const original = fs.readFileSync(path.join(first, 'snapshot.json'));
    const next = path.join(root, 'next');
    const r = spawnSync(
      cli,
      ['run', graph, '--snapshot-dir', next, '--snapshot-max-total-bytes', '1'],
      { encoding: 'utf8' },
    );
    assert.notEqual(r.status, 0);
    assert.match(r.stderr, /quota exceeded/);
    assert.deepEqual(fs.readFileSync(path.join(first, 'snapshot.json')), original);
    assert.equal(fs.existsSync(next), false);
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test('retention is opt-in and expiration is explicit', () => {
  const root = fixture();
  const previous = process.env.LYFLOW_SNAPSHOT_RETENTION_DAYS;
  try {
    const dir = path.join(root, 'snapshots', 'old');
    fs.mkdirSync(dir, { recursive: true });
    const file = path.join(dir, 'snapshot.json');
    fs.writeFileSync(file, JSON.stringify({ snapshotId: 'old', outputs: [] }));
    fs.utimesSync(file, new Date(0), new Date(0));
    delete process.env.LYFLOW_SNAPSHOT_RETENTION_DAYS;
    assert.equal(snapshotStore(root).read('old').manifest.snapshotId, 'old');
    process.env.LYFLOW_SNAPSHOT_RETENTION_DAYS = '1';
    const store = snapshotStore(root);
    assert.throws(
      () => store.read('old'),
      (e) => e.status === 410,
    );
    store.prepare();
    assert.equal(fs.existsSync(dir), false);
  } finally {
    if (previous === undefined) delete process.env.LYFLOW_SNAPSHOT_RETENTION_DAYS;
    else process.env.LYFLOW_SNAPSHOT_RETENTION_DAYS = previous;
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test('geometry is retained as the native value from the same execution', () => {
  const root = fixture();
  try {
    const input = path.join(root, 'plane.pcd');
    fs.writeFileSync(
      input,
      'VERSION 0.7\nFIELDS x y z\nSIZE 4 4 4\nTYPE F F F\nCOUNT 1 1 1\nWIDTH 9\nHEIGHT 1\nPOINTS 9\nDATA ascii\n' +
        Array.from({ length: 9 }, (_, i) => `${i % 3} ${Math.floor(i / 3)} 2`).join('\n') +
        '\n',
    );
    const graph = path.join(root, 'graph.json');
    const graphDoc = doc(input);
    graphDoc.nodes.push({ id: 'fit', op: 'segment.ransac_plane', params: {} });
    graphDoc.edges.push({
      id: 'e',
      from: { node: 'load', port: 'cloud' },
      to: { node: 'fit', port: 'cloud' },
    });
    graphDoc.outputs = { plane: { node: 'fit', port: 'plane' } };
    fs.writeFileSync(graph, JSON.stringify(graphDoc));
    const output = path.join(root, 'snapshot');
    const lines = run(['run', graph, '--snapshot-dir', output, '--outputs']);
    const manifest = JSON.parse(fs.readFileSync(path.join(output, 'snapshot.json')));
    const plane = manifest.outputs.find((o) => o.type === 'Plane');
    assert.ok(plane.value);
    assert.deepEqual(plane.value, lines.at(-1).plane.value);
    assert.equal(plane.missing, undefined);
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test('abandoned pending payloads count toward total quota without automatic deletion', () => {
  const root = fixture();
  try {
    const input = path.join(root, 'input.pcd');
    writePcd(input, 3);
    const graph = path.join(root, 'graph.json');
    fs.writeFileSync(graph, JSON.stringify(doc(input)));
    const pending = path.join(root, 'pending-abandoned');
    fs.mkdirSync(pending);
    fs.writeFileSync(path.join(pending, 'output-0.pcd'), Buffer.alloc(65536));
    const next = path.join(root, 'next');
    const result = spawnSync(
      cli,
      ['run', graph, '--snapshot-dir', next, '--snapshot-max-total-bytes', '32768'],
      { encoding: 'utf8' },
    );
    assert.notEqual(result.status, 0);
    assert.match(result.stderr, /pending captures are counted/);
    assert.equal(fs.statSync(path.join(pending, 'output-0.pcd')).size, 65536);
    assert.equal(fs.existsSync(next), false);
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test('snapshot HTTP storage refuses a directory junction outside its root', () => {
  const root = fixture();
  try {
    const store = snapshotStore(root);
    const outside = path.join(root, 'outside');
    fs.mkdirSync(outside);
    fs.writeFileSync(
      path.join(outside, 'snapshot.json'),
      JSON.stringify({ snapshotId: 'linked', outputs: [] }),
    );
    const link = path.join(root, 'snapshots', 'linked');
    fs.symlinkSync(outside, link, process.platform === 'win32' ? 'junction' : 'dir');
    assert.throws(
      () => store.read('linked'),
      (e) => e.status === 403,
    );
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});
