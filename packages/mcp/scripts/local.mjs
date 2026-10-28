import { spawn } from 'node:child_process';
import { randomBytes } from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const packageRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const args = process.argv.slice(2);
const options = {};
for (let i = 0; i < args.length; i++) {
  if (!['--cli', '--root'].includes(args[i]) || !args[i + 1] || args[i + 1].startsWith('--')) {
    throw new Error('用法：node packages/mcp/scripts/local.mjs --cli <lyflow.exe> --root <工作区>');
  }
  options[args[i].slice(2)] = args[++i];
}
const cli = options.cli || process.env.LYFLOW_CLI;
if (!cli || !fs.statSync(path.resolve(cli), { throwIfNoEntry: false })?.isFile()) {
  throw new Error('请通过 --cli 或 LYFLOW_CLI 指定实际的 LyFlow CLI 文件');
}
const root = path.resolve(options.root || process.env.LYFLOW_HTTP_ROOT || process.cwd());
if (!fs.statSync(root, { throwIfNoEntry: false })?.isDirectory())
  throw new Error(`工作区不存在：${root}`);
const entry = path.join(packageRoot, 'dist/index.js');
if (!fs.existsSync(entry)) throw new Error('请先执行 pnpm --filter @lyflow/mcp build');
const token = randomBytes(24).toString('hex');
const { resolveWorkDir } = await import('../dist/config.js');
const env = {
  ...process.env,
  LYFLOW_CLI: path.resolve(cli),
  LYFLOW_HTTP_TOKEN: token,
  LYFLOW_WORK_DIR: resolveWorkDir(process.env),
};
const backend = spawn(
  process.execPath,
  [
    path.resolve(packageRoot, '../editor/test-server/server.mjs'),
    '--port',
    '0',
    '--root',
    root,
    '--cli',
    env.LYFLOW_CLI,
  ],
  { env, windowsHide: true, stdio: ['ignore', 'pipe', 'pipe', 'ipc'] },
);
backend.stdout.pipe(process.stderr);
backend.stderr.pipe(process.stderr);
let child;
let closing = false;
function stop(code = 0) {
  if (closing) return;
  closing = true;
  child?.kill();
  if (backend.connected) backend.send('shutdown');
  const timer = setTimeout(() => {
    backend.kill();
    process.exit(code);
  }, 2000);
  if (backend.exitCode !== null) {
    clearTimeout(timer);
    process.exit(code);
  }
  backend.once('exit', () => {
    clearTimeout(timer);
    process.exit(code);
  });
}
backend.once('error', (error) => {
  console.error(error.message);
  stop(1);
});
backend.once('exit', (code) => {
  if (!closing) stop(code || 1);
});
process.once('SIGINT', () => stop());
process.once('SIGTERM', () => stop());
process.stdin.once('end', () => stop());
try {
  const port = await new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error('LyFlow HTTP 桥接启动超时')), 15000);
    backend.once('message', (message) => {
      clearTimeout(timer);
      if (message?.type === 'ready' && Number.isInteger(message.port)) resolve(message.port);
      else reject(new Error('LyFlow HTTP 桥接返回无效就绪消息'));
    });
    backend.once('exit', () => {
      clearTimeout(timer);
      reject(new Error('LyFlow HTTP 桥接提前退出'));
    });
  });
  env.LYFLOW_HTTP_BASE = `http://127.0.0.1:${port}`;
  const response = await fetch(`${env.LYFLOW_HTTP_BASE}/lyflow/core-info`, {
    headers: { Authorization: `Bearer ${token}` },
    signal: AbortSignal.timeout(15000),
  });
  if (!response.ok)
    throw new Error(`LyFlow CLI/core 验证失败：HTTP ${response.status} ${await response.text()}`);
  if (!closing) {
    child = spawn(process.execPath, [entry], {
      env,
      windowsHide: true,
      stdio: ['pipe', 'pipe', 'inherit'],
    });
    process.stdin.pipe(child.stdin);
    child.stdout.pipe(process.stdout);
    child.stdin.on('error', (error) => {
      if (!closing) {
        console.error(error.message);
        stop(1);
      }
    });
    child.once('error', (error) => {
      console.error(error.message);
      stop(1);
    });
    child.once('exit', (code) => stop(code || 0));
  }
} catch (error) {
  console.error(error.message);
  stop(1);
}
