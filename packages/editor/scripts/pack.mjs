import { mkdtemp, readFile, writeFile, cp, mkdir, rm, readdir } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { dirname, join, relative, resolve } from 'node:path';
import { tmpdir } from 'node:os';
import { spawnSync } from 'node:child_process';
import { existsSync } from 'node:fs';
const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const at = process.argv.indexOf('--pack-destination');
if (at < 0 || !process.argv[at + 1])
  throw new Error('Usage: node scripts/pack.mjs --pack-destination <directory>');
const destination = resolve(process.argv[at + 1]);
await mkdir(destination, { recursive: true });
const stage = await mkdtemp(join(tmpdir(), 'lyflow-editor-pack-'));
const shared = '@lyflow/visualization';
try {
  const source = join(stage, 'src');
  await cp(join(root, 'src'), source, { recursive: true });
  const vendor = join(source, 'lib/visualization');
  await mkdir(vendor, { recursive: true });
  for (const name of ['index.js', 'index.d.ts'])
    await cp(join(root, '../visualization', name), join(vendor, name));
  // 发布包不带 workspace 依赖：每个源文件对共享可视化包的引用都指到随包副本。
  let rewritten = 0;
  for (const entry of await readdir(source, { recursive: true, withFileTypes: true })) {
    if (!entry.isFile() || !/\.tsx?$/.test(entry.name)) continue;
    const file = join(entry.parentPath, entry.name);
    const text = await readFile(file, 'utf8');
    if (!text.includes(shared)) continue;
    let target = relative(dirname(file), join(vendor, 'index.js')).replaceAll('\\', '/');
    if (!target.startsWith('.')) target = `./${target}`;
    const next = text.replaceAll(`'${shared}'`, `'${target}'`);
    if (next.includes(shared))
      throw new Error(`Unsupported reference to ${shared} in ${relative(source, file)}`);
    await writeFile(file, next);
    rewritten += 1;
  }
  if (rewritten === 0) throw new Error(`No reference to ${shared} found in the editor sources`);
  const pkg = JSON.parse(await readFile(join(root, 'package.json'), 'utf8'));
  delete pkg.dependencies[shared];
  delete pkg.scripts;
  await writeFile(join(stage, 'package.json'), JSON.stringify(pkg, null, 2) + '\n');
  const args = ['pack', stage, '--pack-destination', destination, '--silent'];
  let executable = 'npm';
  if (process.platform === 'win32') {
    const npmCommands = spawnSync('where.exe', ['npm.cmd'], {
      encoding: 'utf8',
      windowsHide: true,
    });
    const candidates = [
      join(dirname(process.execPath), 'node_modules/npm/bin/npm-cli.js'),
      ...(npmCommands.stdout ?? '')
        .trim()
        .split(/\r?\n/)
        .filter(Boolean)
        .map((p) => join(dirname(p), 'node_modules/npm/bin/npm-cli.js')),
    ];
    const cli = candidates.find(existsSync);
    if (!cli) throw new Error('Cannot locate npm-cli.js beside Node.js or npm.cmd');
    executable = process.execPath;
    args.unshift(cli);
  }
  const result = spawnSync(executable, args, { stdio: 'inherit', windowsHide: true });
  if (result.error) throw result.error;
  if (result.status !== 0) throw new Error(`npm pack exited ${result.status}`);
} finally {
  await rm(stage, { recursive: true, force: true });
}
