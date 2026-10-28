import fs from 'node:fs';
import path from 'node:path';
import { randomUUID } from 'node:crypto';

const error = (message, status = 404) => Object.assign(new Error(message), { status });
export function snapshotStore(workDir) {
  const requestedRoot = path.join(workDir, 'snapshots');
  fs.mkdirSync(requestedRoot, { recursive: true });
  const root = fs.realpathSync(requestedRoot);
  const maxTotalBytes = Number(process.env.LYFLOW_SNAPSHOT_MAX_TOTAL_BYTES ?? 10737418240);
  const retentionMs = Number(process.env.LYFLOW_SNAPSHOT_RETENTION_DAYS ?? 0) * 86400000;
  if (
    !Number.isSafeInteger(maxTotalBytes) ||
    maxTotalBytes < 0 ||
    !Number.isFinite(retentionMs) ||
    retentionMs < 0
  )
    throw error('Invalid snapshot storage limits', 500);
  function safeFile(dir, name) {
    const relative = path.relative(root, dir);
    if (
      !relative ||
      relative.startsWith('..') ||
      path.isAbsolute(relative) ||
      path.dirname(relative) !== '.'
    )
      throw error('Invalid snapshot directory', 403);
    if (fs.lstatSync(dir).isSymbolicLink())
      throw error('Snapshot directory links are not allowed', 403);
    const resolvedDir = fs.realpathSync(dir);
    if (path.relative(root, resolvedDir) !== relative)
      throw error('Snapshot directory escapes storage', 403);
    const file = path.join(resolvedDir, name);
    if (fs.lstatSync(file).isSymbolicLink())
      throw error('Snapshot file links are not allowed', 403);
    const resolvedFile = fs.realpathSync(file);
    if (path.dirname(resolvedFile) !== resolvedDir)
      throw error('Snapshot file escapes storage', 403);
    return resolvedFile;
  }
  function expired(dir) {
    return (
      retentionMs > 0 &&
      Date.now() - fs.statSync(safeFile(dir, 'snapshot.json')).mtimeMs > retentionMs
    );
  }
  function prune() {
    if (!retentionMs) return;
    for (const entry of fs.readdirSync(root, { withFileTypes: true })) {
      if (!entry.isDirectory() || !/^[A-Za-z0-9_-]+$/.test(entry.name)) continue;
      const dir = directory(entry.name);
      if (fs.existsSync(path.join(dir, 'snapshot.json')) && expired(dir))
        fs.rmSync(dir, { recursive: true });
    }
  }
  function directory(id) {
    if (!/^[A-Za-z0-9_-]+$/.test(id)) throw error('Invalid snapshot id', 400);
    return path.join(root, id);
  }
  function read(id) {
    const dir = directory(id);
    try {
      if (expired(dir)) throw error(`Snapshot expired: ${id}`, 410);
      return {
        dir,
        manifest: JSON.parse(fs.readFileSync(safeFile(dir, 'snapshot.json'), 'utf8')),
      };
    } catch (e) {
      if (e.code === 'ENOENT') throw error(`Snapshot unavailable: ${id}`);
      throw e;
    }
  }
  return {
    read,
    prepare(capture = { policy: 'all' }) {
      prune();
      if (!capture || !['all', 'selected', 'failures', 'metrics'].includes(capture.policy))
        throw error('Invalid capture policy', 400);
      const maxBytes = capture.maxBytes ?? 268435456;
      if (!Number.isSafeInteger(maxBytes) || maxBytes < 0)
        throw error('capture.maxBytes must be a nonnegative integer', 400);
      const outputs = capture.outputs ?? [];
      if (
        !Array.isArray(outputs) ||
        outputs.some(
          (o) =>
            !o ||
            typeof o.nodeId !== 'string' ||
            !o.nodeId ||
            o.nodeId.includes(':') ||
            typeof o.port !== 'string' ||
            !o.port,
        )
      )
        throw error('Invalid capture.outputs', 400);
      if (capture.policy === 'selected' && !outputs.length)
        throw error('selected capture requires outputs', 400);
      const dir = path.join(root, `pending-${randomUUID()}`);
      const argv = [
        '--snapshot-dir',
        dir,
        '--capture-policy',
        capture.policy,
        '--capture-max-bytes',
        String(maxBytes),
        '--snapshot-max-total-bytes',
        String(maxTotalBytes),
      ];
      for (const o of outputs) argv.push('--capture-output', `${o.nodeId}:${o.port}`);
      return { dir, argv };
    },
    finish(pending, runId) {
      if (!fs.existsSync(path.join(pending.dir, 'snapshot.json'))) return false;
      const manifest = JSON.parse(fs.readFileSync(safeFile(pending.dir, 'snapshot.json'), 'utf8'));
      if (manifest.runId !== runId || manifest.snapshotId !== runId)
        throw error('Snapshot identity mismatch', 500);
      const dest = directory(runId);
      if (fs.existsSync(dest)) throw error('Snapshot already exists', 409);
      fs.renameSync(pending.dir, dest);
      return true;
    },
    output(id, nodeId, port, type) {
      const { dir, manifest } = read(id);
      const output = manifest.outputs.find((o) => o.nodeId === nodeId && o.port === port);
      if (!output || output.missing || !output.file)
        throw error(
          `Output not captured: ${nodeId}:${port} (${output?.missing ?? 'output_unavailable'})`,
          410,
        );
      if (output.type !== type) throw error(`Expected ${type}, got ${output.type}`, 400);
      if (path.basename(output.file) !== output.file) throw error('Invalid snapshot file', 500);
      return safeFile(dir, output.file);
    },
  };
}
