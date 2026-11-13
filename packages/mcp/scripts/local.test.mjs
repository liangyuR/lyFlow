import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import test from 'node:test';

const launcher = path.join(path.dirname(fileURLToPath(import.meta.url)), 'local.mjs');
test('缺失CLI明确失败且不污染MCP标准输出', () => {
  const result = spawnSync(
    process.execPath,
    [launcher, '--cli', path.join(import.meta.dirname, 'missing.exe')],
    { encoding: 'utf8' },
  );
  assert.notEqual(result.status, 0);
  assert.equal(result.stdout, '');
  assert.match(result.stderr, /LyFlow CLI/);
});
test('拒绝未知参数', () => {
  const result = spawnSync(process.execPath, [launcher, '--unknown'], { encoding: 'utf8' });
  assert.notEqual(result.status, 0);
  assert.equal(result.stdout, '');
  assert.match(result.stderr, /用法/);
});
