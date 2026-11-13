import { mkdtemp, readFile, writeFile, cp, mkdir, rm } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { dirname, join, resolve } from 'node:path';
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
try {
  await cp(join(root, 'src'), join(stage, 'src'), { recursive: true });
  const vendor = join(stage, 'src/lib/visualization');
  await mkdir(vendor, { recursive: true });
  for (const name of ['index.js', 'index.d.ts'])
    await cp(join(root, '../visualization', name), join(vendor, name));
  for (const name of ['shapes2d.ts', 'sceneExport.ts']) {
    const file = join(stage, 'src/lib', name);
    const source = await readFile(file, 'utf8');
    if (!source.includes("from '@lyflow/visualization'"))
      throw new Error(`Shared visualization import missing in ${name}`);
    await writeFile(
      file,
      source.replace("from '@lyflow/visualization'", "from './visualization/index.js'"),
    );
  }
  const pkg = JSON.parse(await readFile(join(root, 'package.json'), 'utf8'));
  delete pkg.dependencies['@lyflow/visualization'];
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
